// Copyright (c) SplatterVault. Licensed under the MIT License.

package com.splattervault.sdk;

import android.app.Activity;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Seals the SplatterVault SDK's stored secrets (refresh tokens, guest secrets, pending store changes) with an
 * AES-256-GCM key kept in the Android Keystore: the key never leaves the keystore, and a sealed value opens only on
 * this device, for this app. Called from C++ (SvSecureStore.cpp) through JNI; every method answers null on failure
 * and never logs a value.
 */
public final class SvKeystore {
	private static final String PROVIDER = "AndroidKeyStore";
	private static final String ALIAS = "splattervault.store.v1";
	private static final String TRANSFORM = "AES/GCM/NoPadding";
	private static final byte VERSION = 1;
	private static final int IV_BYTES = 12;
	private static final int TAG_BITS = 128;

	/** The key, looked up once: a KeyStore load on every call would cost each sealed read or write on the game thread. */
	private static volatile SecretKey cachedKey;

	private SvKeystore() {
	}

	private static synchronized SecretKey key() throws Exception {
		SecretKey cached = cachedKey;
		if (cached != null) return cached;
		cachedKey = loadOrCreateKey();
		return cachedKey;
	}

	private static SecretKey loadOrCreateKey() throws Exception {
		KeyStore store = KeyStore.getInstance(PROVIDER);
		store.load(null);
		KeyStore.Entry entry = store.getEntry(ALIAS, null);
		if (entry instanceof KeyStore.SecretKeyEntry) {
			return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
		}
		KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, PROVIDER);
		generator.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
			.setBlockModes(KeyProperties.BLOCK_MODE_GCM)
			.setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
			.setKeySize(256)
			.setRandomizedEncryptionRequired(true)
			.build());
		return generator.generateKey();
	}

	/** [version][12-byte IV][ciphertext + tag], bound to aad (the stored key's name); null on failure. */
	public static byte[] protect(byte[] data, byte[] aad) {
		if (data == null) return null; // never seal "nothing" as an empty value
		try {
			Cipher cipher = Cipher.getInstance(TRANSFORM);
			cipher.init(Cipher.ENCRYPT_MODE, key());
			if (aad != null) cipher.updateAAD(aad);
			byte[] iv = cipher.getIV();
			byte[] sealed = cipher.doFinal(data);
			if (iv == null || iv.length != IV_BYTES) return null;
			byte[] out = new byte[1 + IV_BYTES + sealed.length];
			out[0] = VERSION;
			System.arraycopy(iv, 0, out, 1, IV_BYTES);
			System.arraycopy(sealed, 0, out, 1 + IV_BYTES, sealed.length);
			return out;
		} catch (Throwable t) {
			return null;
		}
	}

	/** The value, or null when the blob does not open (another device or app, a reset keystore, tampering). */
	public static byte[] unprotect(byte[] blob, byte[] aad) {
		try {
			if (blob == null || blob.length < 1 + IV_BYTES + TAG_BITS / 8 || blob[0] != VERSION) return null;
			Cipher cipher = Cipher.getInstance(TRANSFORM);
			cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(TAG_BITS, blob, 1, IV_BYTES));
			if (aad != null) cipher.updateAAD(aad);
			return cipher.doFinal(blob, 1 + IV_BYTES, blob.length - 1 - IV_BYTES);
		} catch (Throwable t) {
			// A key the keystore dropped (a reset, a restored backup) cannot open anything: look it up again next time.
			cachedKey = null;
			return null;
		}
	}

	/** The app's private internal files directory (Context.getFilesDir()), or null. */
	public static String filesDir(Activity activity) {
		try {
			return activity.getFilesDir().getAbsolutePath();
		} catch (Throwable t) {
			return null;
		}
	}
}
