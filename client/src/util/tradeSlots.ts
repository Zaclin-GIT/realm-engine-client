// Shared trade-slot helpers — used by the dashboard proxy trade observer (DevServer)
// and the SDK script trade bridge (scripts/bridge/trade). Pure functions.

export function normalizeSlotCount(value: unknown, fallback: number): number {
  const parsed = Number(value);
  if (Number.isFinite(parsed)) {
    const count = Math.trunc(parsed);
    if (count >= 1 && count <= 20) return count;
  }
  const fallbackParsed = Number(fallback);
  if (Number.isFinite(fallbackParsed)) {
    const fallbackCount = Math.trunc(fallbackParsed);
    if (fallbackCount >= 1 && fallbackCount <= 20) return fallbackCount;
  }
  return 12;
}

export function toBoolArray(value: unknown, count: number): boolean[] {
  const normalizedCount = normalizeSlotCount(count, 12);
  const out = new Array<boolean>(normalizedCount).fill(false);
  if (!Array.isArray(value)) return out;
  const max = Math.min(value.length, normalizedCount);
  for (let i = 0; i < max; i++) out[i] = Boolean(value[i]);
  return out;
}

export function extractTradeItemIncluded(items: unknown[]): boolean[] {
  const out: boolean[] = [];
  for (const item of items) {
    if (item && typeof item === 'object' && 'included' in item) {
      out.push(Boolean((item as Record<string, unknown>).included));
    } else {
      out.push(false);
    }
  }
  return out;
}

export function parseOfferSlots(raw: string, count: number): boolean[] {
  const normalizedCount = normalizeSlotCount(count, 12);
  const out = new Array<boolean>(normalizedCount).fill(false);
  const trimmed = raw.trim();
  if (!trimmed) return out;
  if (trimmed === '*' || trimmed.toLowerCase() === 'all') {
    return new Array<boolean>(normalizedCount).fill(true);
  }

  const parts = trimmed.split(',').map((p) => p.trim()).filter(Boolean);
  if (!parts.length) return out;
  for (const part of parts) {
    if (!/^\d+$/.test(part)) {
      throw new Error(`Invalid slot value "${part}". Use comma-separated indexes like 0,2,5 or "all".`);
    }
    const idx = Number(part);
    if (!Number.isInteger(idx) || idx < 0 || idx >= normalizedCount) {
      throw new Error(`Slot index ${idx} is out of range (0-${normalizedCount - 1}).`);
    }
    out[idx] = true;
  }
  return out;
}
