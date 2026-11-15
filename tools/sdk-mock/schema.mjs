// A small JSON-schema validator for the subset studio-api.json (OpenAPI 3.0.1) uses: $ref, type (incl. integer),
// properties, required, items, enum, nullable, oneOf / anyOf / allOf, additionalProperties, pattern, format
// (uuid, date-time, email, int32, int64), minimum / maximum, minLength / maxLength, minItems / maxItems.
// No dependencies. validate(spec, schema, value) -> list of "path: problem" strings (empty = valid).

const FORMATS = {
  uuid: (v) => /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(v),
  "date-time": (v) => /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})$/.test(v) && !Number.isNaN(Date.parse(v)),
  email: (v) => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v),
  int32: (v) => Number.isInteger(v) && v >= -2147483648 && v <= 2147483647,
  int64: (v) => Number.isSafeInteger(v)
};

function resolveRef(spec, ref) {
  if (!ref.startsWith("#/")) throw new Error("Only local $ref is supported: " + ref);
  return ref.slice(2).split("/").reduce((o, k) => (o == null ? undefined : o[k.replace(/~1/g, "/").replace(/~0/g, "~")]), spec);
}

function typeOf(v) {
  if (v === null) return "null";
  if (Array.isArray(v)) return "array";
  if (typeof v === "number") return Number.isInteger(v) ? "integer" : "number";
  return typeof v;
}

export function validate(spec, schema, value, path = "$", depth = 0) {
  const errors = [];
  if (depth > 64) return [path + ": schema too deep"];
  if (schema === undefined || schema === true || schema === null) return errors;
  if (schema === false) return [path + ": no value allowed here"];
  if (schema.$ref) {
    const target = resolveRef(spec, schema.$ref);
    if (!target) return [path + ": unresolvable $ref " + schema.$ref];
    // OpenAPI 3.0: siblings of $ref are ignored.
    return validate(spec, target, value, path, depth + 1);
  }

  if (value === null) {
    if (schema.nullable === true) return errors;
    // A schema with no type and no composition accepts null only when nothing restricts it.
    const restrictive = schema.type || schema.allOf || schema.oneOf || schema.anyOf || schema.enum || schema.properties;
    if (restrictive) {
      // allOf members may carry nullable themselves; only accept when every member allows null.
      if (schema.allOf && !schema.type && schema.allOf.every((s) => validate(spec, s, null, path, depth + 1).length === 0)) return errors;
      return [path + ": null is not allowed"];
    }
    return errors;
  }

  if (schema.type) {
    const t = typeOf(value);
    const ok = schema.type === t || (schema.type === "number" && t === "integer");
    if (!ok) return [path + `: expected ${schema.type}, got ${t}`];
  }
  if (schema.enum && !schema.enum.some((e) => e === value)) errors.push(path + `: ${JSON.stringify(value)} is not one of ${JSON.stringify(schema.enum)}`);

  if (typeof value === "string") {
    if (schema.pattern && !new RegExp(schema.pattern, "u").test(value)) errors.push(path + `: ${JSON.stringify(value)} does not match ${schema.pattern}`);
    if (schema.maxLength !== undefined && [...value].length > schema.maxLength) errors.push(path + `: longer than ${schema.maxLength}`);
    if (schema.minLength !== undefined && [...value].length < schema.minLength) errors.push(path + `: shorter than ${schema.minLength}`);
    if (schema.format && FORMATS[schema.format] && !["int32", "int64"].includes(schema.format) && !FORMATS[schema.format](value)) {
      errors.push(path + `: ${JSON.stringify(value)} is not a ${schema.format}`);
    }
  }
  if (typeof value === "number") {
    if (schema.minimum !== undefined && value < schema.minimum) errors.push(path + `: below minimum ${schema.minimum}`);
    if (schema.maximum !== undefined && value > schema.maximum) errors.push(path + `: above maximum ${schema.maximum}`);
    if ((schema.format === "int32" || schema.format === "int64") && !FORMATS[schema.format](value)) errors.push(path + `: not an ${schema.format}`);
  }
  if (Array.isArray(value)) {
    if (schema.minItems !== undefined && value.length < schema.minItems) errors.push(path + `: fewer than ${schema.minItems} items`);
    if (schema.maxItems !== undefined && value.length > schema.maxItems) errors.push(path + `: more than ${schema.maxItems} items`);
    if (schema.items) value.forEach((v, i) => errors.push(...validate(spec, schema.items, v, `${path}[${i}]`, depth + 1)));
  }
  if (typeOf(value) === "object") {
    for (const r of schema.required ?? []) if (!(r in value)) errors.push(path + `: missing required "${r}"`);
    const props = schema.properties ?? {};
    for (const [k, v] of Object.entries(value)) {
      if (props[k] !== undefined) errors.push(...validate(spec, props[k], v, `${path}.${k}`, depth + 1));
      else if (schema.additionalProperties === false) errors.push(path + `: unexpected property "${k}"`);
      else if (schema.additionalProperties && typeof schema.additionalProperties === "object") {
        errors.push(...validate(spec, schema.additionalProperties, v, `${path}.${k}`, depth + 1));
      }
    }
  }
  if (schema.allOf) for (const s of schema.allOf) errors.push(...validate(spec, s, value, path, depth + 1));
  if (schema.anyOf && !schema.anyOf.some((s) => validate(spec, s, value, path, depth + 1).length === 0)) {
    errors.push(path + ": matches none of anyOf");
  }
  if (schema.oneOf) {
    const n = schema.oneOf.filter((s) => validate(spec, s, value, path, depth + 1).length === 0).length;
    if (n !== 1) errors.push(path + `: matches ${n} of oneOf (exactly 1 needed)`);
  }
  return errors;
}

/** Finds the operation for a method + concrete path (without the /rest prefix) in an OpenAPI document. */
export function findOperation(spec, method, path) {
  const m = method.toLowerCase();
  let best = null;
  for (const [template, item] of Object.entries(spec.paths)) {
    if (!item[m]) continue;
    const re = new RegExp("^" + template.replace(/[.*+?^$()|[\]\\]/g, "\\$&").replace(/\\?\{[^}]+\\?\}/g, "[^/]+").replace(/\{[^}]+\}/g, "[^/]+") + "$");
    if (re.test(path)) {
      const literal = template.replace(/\{[^}]+\}/g, "").length;
      if (!best || literal > best.literal) best = { template, op: item[m], literal };
    }
  }
  return best;
}
