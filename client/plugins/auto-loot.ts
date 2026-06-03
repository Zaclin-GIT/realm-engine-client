import type { PluginContext } from '../src/plugins/PluginContext.js';
import type { ClientConnection } from '../src/proxy/ClientConnection.js';
import type { PlayerClassStatMaxes } from '../src/game-data/GameDataLoader.js';
import { StatType } from '../src/constants/StatType.js';
import type { TrackedEntity } from '../src/state/GameWorldState.js';
import { readFileSync, existsSync, writeFileSync } from 'fs';
import { join } from 'path';
import { homedir } from 'os';

interface ItemInfo {
  itemId: number;
  name: string;
  slotType: number;
  tier: number | null;
  isUT: boolean;
  isST: boolean;
  quickslotAllowed: boolean;
}

type GearCategory = 'weapon' | 'ability' | 'armor' | 'ring';

interface LootCatalog {
  items: Map<number, ItemInfo>;
}

interface AutoLootState {
  lastPickupAt: number;
  recentAttempts: Map<string, number>;
  pendingDestSlotId: number | null;
  pendingDestQuantity: number | null;
  pendingPotionItemId: number | null;
  pendingSince: number;
  bagSeenAt: Map<number, number>;
  notifiedBagIds: Set<number>;
  lastPos: { x: number; y: number } | null;
  stationaryTicks: number;
  reservedDestSlots: Map<number, number>;
  consumedBagSlots: Map<string, number>;
  manualPotionSuppressUntil: number;
  lastManualPotionSuppressLogAt: number;
  manualPotionPacketBlockUntil: number;
  lastManualPotionPacketBlockLogAt: number;
}

interface LootDestination {
  packetSlotId: number;
  currentObjectType: number;
}

const DEFAULT_MIN_WEAPON_TIER = 11;
const DEFAULT_MIN_ABILITY_TIER = 6;
const DEFAULT_MIN_ARMOR_TIER = 11;
const DEFAULT_MIN_RING_TIER = 6;

const PICKUP_INTERVAL_MS = 600;
const RETRY_ITEM_AFTER_MS = 1500;
const PENDING_DEST_TIMEOUT_MS = 1200;
const DEST_SLOT_RESERVE_MS = 30000;
const BAG_SLOT_CONSUME_MS = 30000;
const ON_TOP_DISTANCE = 1.0;
const PUBLIC_BAG_DELAY_MS = 2000;
const STATIONARY_TICK_LIMIT = 100;
const MOVEMENT_EPSILON = 0.05;
const QUICKSLOT_PACKET_BASE = 1000000;
const QUICK_SLOT_COUNT = 3;
const MAX_HP_QUICKSLOT_STACK = 6;
const MAX_MP_QUICKSLOT_STACK = 6;
const BAG_NOTIFY_RADIUS = 16;
const BAG_NOTIFY_ITEM_LIMIT = 8;
const DEFAULT_MANUAL_POTION_SUPPRESS_MS = 4000;
const MIN_MANUAL_POTION_SUPPRESS_MS = 1000;
const MAX_MANUAL_POTION_SUPPRESS_MS = 12000;
const MANUAL_POTION_SUPPRESS_LOG_MS = 1500;
const DEFAULT_MANUAL_POTION_PACKET_BLOCK_MS = 1200;
const MIN_MANUAL_POTION_PACKET_BLOCK_MS = 300;
const MAX_MANUAL_POTION_PACKET_BLOCK_MS = 3000;
const MANUAL_POTION_PACKET_BLOCK_LOG_MS = 500;

const BAG_TYPES = new Set<number>([
  1280, 1281, 1283, 1286, 1287, 1288, 1289, 1291, 1292, 1294, 1295, 1296,
  1708, 1709, 1710, 1722, 1723, 1724, 1725, 1726, 1727, 1728, 8239,
]);

/** Multitool `AutoLootBigBags` / `Class88.method_2` on `UPDATE` newObjs: force `Size` stat value for bag types. */
const MULTITOOL_BIG_BAG_SIZE = 175;

function toStatInt(value: unknown): number {
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  return Math.trunc(n);
}

const PUBLIC_BAG_TYPES = new Set<number>([
  1280, 1281, 1286, 1709, 1710, 8239,
]);

const WEAPON_SLOT_TYPES = new Set<number>([1, 2, 3, 8, 17, 24]);
const ABILITY_SLOT_TYPES = new Set<number>([4, 5, 11, 12, 13, 15, 16, 18, 19, 20, 21, 22, 23, 25, 27, 28, 29, 30, 31]);
const ARMOR_SLOT_TYPES = new Set<number>([6, 7, 14]);
const RING_SLOT_TYPES = new Set<number>([9]);

/** Weapon / ability / armor / ring slots -> same buckets as {@link getGearCategory}. */
function isGearSlotType(slotType: number): boolean {
  return WEAPON_SLOT_TYPES.has(slotType)
    || ABILITY_SLOT_TYPES.has(slotType)
    || ARMOR_SLOT_TYPES.has(slotType)
    || RING_SLOT_TYPES.has(slotType);
}

/**
 * Match Multitool `Class22`: explicit `UT`, or **no `<Tier>`** (empty string) defaults to UT for gear.
 * `ST` is never UT. `normalized` must be uppercase trimmed.
 */
function isMultitoolUtTier(normalizedTier: string, slotType: number): boolean {
  if (normalizedTier === 'ST') return false;
  if (normalizedTier === 'UT') return true;
  if (normalizedTier !== '') return false;
  return isGearSlotType(slotType);
}

const HP_POTION_IDS = new Set<number>([2594, 2736]);
const MP_POTION_IDS = new Set<number>([2595, 2781]);
// Permanent life/mana stat increases (including Greater variants)
const LIFE_MANA_POTION_IDS = new Set<number>([
  2793, 2794,   // Potion of Life, Potion of Mana
  5471, 5472,   // Potion of Life (SB), Potion of Mana (SB)
  9070, 9071,   // Greater Potion of Life, Greater Potion of Mana
]);
const STAT_POTION_IDS = new Set<number>([
  2591, 2592, 2593, 2612, 2613, 2636,         // Def/Spd/Att/Wis/Vit/Dex
  5465, 5466, 5467, 5468, 5469, 5470,         // Att/Def/Spd/Vit/Wis/Dex (SB)
  5094,                                         // Mystery Stat Pot
  9064, 9065, 9066, 9067, 9068, 9069,          // Greater Att/Def/Spd/Vit/Wis/Dex
]);

const MYSTERY_STAT_POT_ID = 5094;

type PermanentStatKey = 'attack' | 'defense' | 'speed' | 'dexterity' | 'vitality' | 'wisdom';

/** Which permanent stat each potion raises. See id lines in `STAT_POTION_IDS` comments. */
const STAT_POT_ITEM_TO_PERMANENT: Record<number, PermanentStatKey> = {
  2591: 'defense',
  2592: 'speed',
  2593: 'attack',
  2612: 'wisdom',
  2613: 'vitality',
  2636: 'dexterity',
  5465: 'attack',
  5466: 'defense',
  5467: 'speed',
  5468: 'vitality',
  5469: 'wisdom',
  5470: 'dexterity',
  9064: 'attack',
  9065: 'defense',
  9066: 'speed',
  9067: 'vitality',
  9068: 'wisdom',
  9069: 'dexterity',
};

const PERMANENT_STATS_ALL: readonly PermanentStatKey[] = [
  'attack', 'defense', 'speed', 'dexterity', 'vitality', 'wisdom',
];

function classCapForPermanent(caps: PlayerClassStatMaxes, s: PermanentStatKey): number {
  switch (s) {
    case 'attack': return caps.attack;
    case 'defense': return caps.defense;
    case 'speed': return caps.speed;
    case 'dexterity': return caps.dexterity;
    case 'vitality': return caps.hpRegen; // HpRegen @max in objects.xml
    case 'wisdom': return caps.mpRegen;   // MpRegen @max
    default: return 0;
  }
}

/**
 * `PlayerData.attack` through `wisdom` = permanent stats (class + level + pots); not gear 48+ / exalt.
 * Skip autodrink if that value is already at/above the class 8/8 `max` from objects.xml.
 */
function shouldSkipAutodrinkClassCap(
  classType: number,
  p: { attack: number; defense: number; speed: number; dexterity: number; vitality: number; wisdom: number },
  itemId: number,
  getCaps: (ct: number) => PlayerClassStatMaxes | undefined,
): boolean {
  const caps = getCaps(classType);
  if (!caps) return false;

  if (itemId === MYSTERY_STAT_POT_ID) {
    for (const s of PERMANENT_STATS_ALL) {
      const cap = classCapForPermanent(caps, s);
      if (!Number.isFinite(cap) || cap <= 0) return false;
      if (p[s] < cap) return false;
    }
    return true;
  }

  const target = STAT_POT_ITEM_TO_PERMANENT[itemId];
  if (!target) return false;

  const cap = classCapForPermanent(caps, target);
  if (!Number.isFinite(cap) || cap <= 0) return false;
  const v = p[target];
  return Number.isFinite(v) && v >= cap;
}

/** Stat 80 (`UniqueDataStr`): comma-separated base64 enchant blobs per slot (see `damage-sniffer.ts`). */
const UNIQUE_DATA_STAT_ID = 80;

/**
 *  Decode one base64 segment from stat 80 -> enchant type ids.
 * Wire format: 1-byte header, 2-byte type 0x0402, then 2-byte LE ids until 0xFFFD.
 */
function decodeEnchantIdsFromBlob(code: string): number[] {
  const raw = code.trim();
  if (!raw) return [];
  try {
    const normalized = raw.replace(/-/g, '+').replace(/_/g, '/').padEnd(Math.ceil(raw.length / 4) * 4, '=');
    const bytes = Buffer.from(normalized, 'base64');
    if (bytes.length <= 3) return [];
    const ids: number[] = [];
    for (let pos = 3; pos + 1 < bytes.length; pos += 2) {
      const value = bytes.readUInt16LE(pos);
      if (value === 0xfffd) break;
      ids.push(value === 0xfffe ? 0 : value);
    }
    return ids;
  } catch {
    return [];
  }
}

function getBagSlotEnchantIds(
  stats: Record<string, number | string> | undefined,
  bagSlot: number,
): number[] {
  const v = stats?.[String(UNIQUE_DATA_STAT_ID)];
  if (typeof v !== 'string' || !v.trim()) return [];
  const parts = v.split(',');
  return decodeEnchantIdsFromBlob(parts[bagSlot] ?? '').filter((id) => id > 0);
}

/** Minimum enchant count on the wire: None=0, Uncommon=1, Rare=2, Legendary=3, Divine=4. */
const MIN_ENCHANT_SELECT_VALUES = ['none', 'uncommon', 'rare', 'legendary', 'divine'] as const;

function minEnchantSelectToCount(value: string): number {
  const v = String(value || '').toLowerCase();
  const i = (MIN_ENCHANT_SELECT_VALUES as readonly string[]).indexOf(v);
  return i < 0 ? 0 : i;
}

function loadLootCatalog(ctx: PluginContext): LootCatalog {
  const items = new Map<number, ItemInfo>();
  const objects = ctx.gameData?.getAllObjects() ?? [];

  for (const obj of objects) {
    const itemId = Number(obj.type);
    if (!Number.isFinite(itemId)) continue;

    const slotTypeRaw = Number(obj.slotType ?? -1);
    if (!Number.isFinite(slotTypeRaw) || slotTypeRaw < 0) continue;

    const normalizedTier = String(obj.tierStr ?? '').trim().toUpperCase();
    const isST = normalizedTier === 'ST';
    const isUT = isMultitoolUtTier(normalizedTier, Math.trunc(slotTypeRaw));
    const tier = (isUT || isST || !/^-?\d+$/.test(normalizedTier))
      ? null
      : Number(normalizedTier);

    items.set(itemId, {
      itemId,
      name: String(obj.id || '').trim() || `0x${itemId.toString(16)}`,
      slotType: Math.trunc(slotTypeRaw),
      tier,
      isUT,
      isST,
      quickslotAllowed: obj.quickslotAllowed === true,
    });
  }

  return { items };
}

export function register(ctx: PluginContext) {
  ctx.name = 'Auto Loot';
  ctx.category = 'automation';

  const catalog = loadLootCatalog(ctx);
  const states = new WeakMap<ClientConnection, AutoLootState>();

  let minWeaponTier = DEFAULT_MIN_WEAPON_TIER;
  let minAbilityTier = DEFAULT_MIN_ABILITY_TIER;
  let minArmorTier = DEFAULT_MIN_ARMOR_TIER;
  let minRingTier = DEFAULT_MIN_RING_TIER;

  let lootUTs = true;
  let lootSTs = false;
  let lootHpPotions = false;
  let lootMpPotions = false;
  let lootStatPotions = true;
  let autodrinkStatPots = false;
  let lootLifeManaPotions = true;
  let lootMarks = false;
  let lootEggs = false;
  let publicDelay = true;
  let disableWhenIdle = true;
  let useBackpack = true;
  let preferBackpack = false;
  let restockQuickSlots = true;
  let manualPotionSuppressMs = DEFAULT_MANUAL_POTION_SUPPRESS_MS;
  let manualPotionPacketBlockMs = DEFAULT_MANUAL_POTION_PACKET_BLOCK_MS;
  let bagNotifierEnabled = false;
  let bigLootBags = false;
  let minEnchantTier = 0;

  let whitelist = new Set<number>();
  let blacklist = new Set<number>();

  function getRealmengineDir(): string {
    return join(process.env.USERPROFILE || homedir(), 'Documents', 'Realmengine');
  }

  function parseListFile(filePath: string, listName: string): Set<number> {
    if (!existsSync(filePath)) return new Set();
    try {
      const raw = JSON.parse(readFileSync(filePath, 'utf8')) as unknown;
      const arr: unknown[] = Array.isArray(raw) ? raw
        : (raw !== null && typeof raw === 'object' && Array.isArray((raw as Record<string, unknown>).items))
          ? (raw as Record<string, unknown>).items as unknown[]
          : [];
      const ids = new Set<number>();
      for (const entry of arr) {
        const id = typeof entry === 'number' ? entry
          : (entry !== null && typeof entry === 'object') ? Number((entry as Record<string, unknown>).id)
          : NaN;
        if (Number.isFinite(id) && id > 0) ids.add(Math.trunc(id));
      }
      return ids;
    } catch (err) {
      ctx.log(`Auto Loot: failed to load ${listName}: ${(err as Error).message}`);
      return new Set();
    }
  }

  function reloadLists(dashboard = false): void {
    const dir = getRealmengineDir();
    whitelist = parseListFile(join(dir, 'autoloot-whitelist.json'), 'whitelist');
    blacklist = parseListFile(join(dir, 'autoloot-blacklist.json'), 'blacklist');
    const msg = `Whitelist: ${whitelist.size} item(s) | Blacklist: ${blacklist.size} item(s)`;
    if (dashboard) ctx.dashboardLog(msg); else ctx.log(msg);
  }

  reloadLists();

  ctx.registerSetting('minWeaponTier', {
    label: 'Min Weapon Tier',
    type: 'number',
    value: minWeaponTier,
    min: 0,
    max: 20,
    step: 1,
  }, (value: number) => {
    minWeaponTier = Math.max(0, Math.min(20, Math.trunc(Number(value) || 0)));
  });

  ctx.registerSetting('minAbilityTier', {
    label: 'Min Ability Tier',
    type: 'number',
    value: minAbilityTier,
    min: 0,
    max: 20,
    step: 1,
  }, (value: number) => {
    minAbilityTier = Math.max(0, Math.min(20, Math.trunc(Number(value) || 0)));
  });

  ctx.registerSetting('minArmorTier', {
    label: 'Min Armor Tier',
    type: 'number',
    value: minArmorTier,
    min: 0,
    max: 20,
    step: 1,
  }, (value: number) => {
    minArmorTier = Math.max(0, Math.min(20, Math.trunc(Number(value) || 0)));
  });

  ctx.registerSetting('minRingTier', {
    label: 'Min Ring Tier',
    type: 'number',
    value: minRingTier,
    min: 0,
    max: 20,
    step: 1,
  }, (value: number) => {
    minRingTier = Math.max(0, Math.min(20, Math.trunc(Number(value) || 0)));
  });

  ctx.registerSetting('lootUTs', {
    label: 'Loot UTs',
    type: 'boolean',
    value: lootUTs,
  }, (value: boolean) => {
    lootUTs = value === true;
  });

  ctx.registerSetting('lootSTs', {
    label: 'Loot STs',
    type: 'boolean',
    value: lootSTs,
  }, (value: boolean) => {
    lootSTs = value === true;
  });

  ctx.registerSetting('lootHpPotions', {
    label: 'Loot HP Pots',
    type: 'boolean',
    value: lootHpPotions,
  }, (value: boolean) => {
    lootHpPotions = value === true;
  });

  ctx.registerSetting('lootMpPotions', {
    label: 'Loot MP Pots',
    type: 'boolean',
    value: lootMpPotions,
  }, (value: boolean) => {
    lootMpPotions = value === true;
  });

  ctx.registerSetting('lootStatPotions', {
    label: 'Loot Stat Pots',
    type: 'boolean',
    value: lootStatPotions,
  }, (value: boolean) => {
    lootStatPotions = value === true;
  });

  ctx.registerSetting('autodrinkStatPots', {
    label: 'Autodrink Stat Pots (USEITEM from bag, 0,0 - no loot)', advanced: true,
    type: 'boolean',
    value: autodrinkStatPots,
  }, (value: boolean) => {
    autodrinkStatPots = value === true;
  });

  ctx.registerSetting('minEnchant', {
    label: 'Min enchant (non-potions)', advanced: true,
    type: 'select',
    value: 'none',
    options: [
            { label: 'None', value: 'none' },
      { label: 'Uncommon (>=1 enchant)', value: 'uncommon' },
      { label: 'Rare (>=2)', value: 'rare' },
      { label: 'Legendary (>=3)', value: 'legendary' },
      { label: 'Divine (>=4)', value: 'divine' },
    ],
  }, (value: string) => {
    minEnchantTier = minEnchantSelectToCount(value);
  });

  ctx.registerSetting('lootLifeManaPotions', {
    label: 'Loot Life/Mana Pots', advanced: true,
    type: 'boolean',
    value: lootLifeManaPotions,
  }, (value: boolean) => {
    lootLifeManaPotions = value === true;
  });

  ctx.registerSetting('lootMarks', {
    label: 'Loot Marks',
    type: 'boolean',
    value: lootMarks,
  }, (value: boolean) => {
    lootMarks = value === true;
  });

  ctx.registerSetting('lootEggs', {
    label: 'Loot Eggs',
    type: 'boolean',
    value: lootEggs,
  }, (value: boolean) => {
    lootEggs = value === true;
  });

  ctx.registerSetting('publicDelay', {
    label: 'Public Delay', advanced: true,
    type: 'boolean',
    value: publicDelay,
  }, (value: boolean) => {
    publicDelay = value === true;
  });

  ctx.registerSetting('disableWhenIdle', {
    label: 'Disable When Idle', advanced: true,
    type: 'boolean',
    value: disableWhenIdle,
  }, (value: boolean) => {
    disableWhenIdle = value === true;
  });

  ctx.registerSetting('useBackpack', {
    label: 'Use Backpack', advanced: true,
    type: 'boolean',
    value: useBackpack,
  }, (value: boolean) => {
    useBackpack = value === true;
  });

  ctx.registerSetting('preferBackpack', {
    label: 'Prefer Backpack', advanced: true,
    type: 'boolean',
    value: preferBackpack,
  }, (value: boolean) => {
    preferBackpack = value === true;
  });

  ctx.registerSetting('restockQuickSlots', {
    label: 'Restock Quickslots', advanced: true,
    type: 'boolean',
    value: restockQuickSlots,
  }, (value: boolean) => {
    restockQuickSlots = value === true;
  });

  ctx.registerSetting('manualPotionPauseSeconds', {
    label: 'Manual Potion Pause (seconds)', advanced: true,
    type: 'number',
    value: manualPotionSuppressMs / 1000,
    min: MIN_MANUAL_POTION_SUPPRESS_MS / 1000,
    max: MAX_MANUAL_POTION_SUPPRESS_MS / 1000,
    step: 0.5,
  }, (value: number) => {
    const seconds = Number(value);
    const ms = Math.trunc((Number.isFinite(seconds) ? seconds : DEFAULT_MANUAL_POTION_SUPPRESS_MS / 1000) * 1000);
    manualPotionSuppressMs = Math.max(
      MIN_MANUAL_POTION_SUPPRESS_MS,
      Math.min(MAX_MANUAL_POTION_SUPPRESS_MS, ms),
    );
  });

  ctx.registerSetting('manualPotionPacketBlockSeconds', {
    label: 'Manual Potion Packet Block (seconds)', advanced: true,
    type: 'number',
    value: manualPotionPacketBlockMs / 1000,
    min: MIN_MANUAL_POTION_PACKET_BLOCK_MS / 1000,
    max: MAX_MANUAL_POTION_PACKET_BLOCK_MS / 1000,
    step: 0.1,
  }, (value: number) => {
    const seconds = Number(value);
    const ms = Math.trunc((Number.isFinite(seconds) ? seconds : DEFAULT_MANUAL_POTION_PACKET_BLOCK_MS / 1000) * 1000);
    manualPotionPacketBlockMs = Math.max(
      MIN_MANUAL_POTION_PACKET_BLOCK_MS,
      Math.min(MAX_MANUAL_POTION_PACKET_BLOCK_MS, ms),
    );
  });

  ctx.registerSetting('toggleBagNotifier', {
    label: 'Bag Notifier: Off', advanced: true,
    type: 'button',
    value: null,
  }, () => {
    bagNotifierEnabled = !bagNotifierEnabled;
    const setting = ctx.getSettings().find((entry) => entry.key === 'toggleBagNotifier');
    if (setting) {
      setting.label = `Bag Notifier: ${bagNotifierEnabled ? 'On' : 'Off'}`;
    }
    ctx.log(`Bag notifier ${bagNotifierEnabled ? 'enabled' : 'disabled'}.`);
  });

  ctx.registerSetting('bigLootBags', {
    label: 'Big Loot Bags', advanced: true,
    type: 'boolean',
    value: false,
  }, (value: boolean) => {
    bigLootBags = value === true;
  });

  // Hidden settings used by the edit-list modal to save content back to disk
  ctx.registerSetting('_saveWhitelist', {
    label: '_saveWhitelist', type: 'text', value: '', hidden: true,
  }, (value: string) => {
    try {
      JSON.parse(value);
      writeFileSync(join(getRealmengineDir(), 'autoloot-whitelist.json'), value, 'utf8');
      reloadLists(true);
    } catch (err) {
      ctx.dashboardLog(`Whitelist save failed: ${(err as Error).message}`);
    }
  });

  ctx.registerSetting('_saveBlacklist', {
    label: '_saveBlacklist', type: 'text', value: '', hidden: true,
  }, (value: string) => {
    try {
      JSON.parse(value);
      writeFileSync(join(getRealmengineDir(), 'autoloot-blacklist.json'), value, 'utf8');
      reloadLists(true);
    } catch (err) {
      ctx.dashboardLog(`Blacklist save failed: ${(err as Error).message}`);
    }
  });

  function readListFile(name: 'whitelist' | 'blacklist'): string {
    const p = join(getRealmengineDir(), `autoloot-${name}.json`);
    return existsSync(p) ? readFileSync(p, 'utf8') : '[]';
  }

  ctx.registerSetting('editWhitelist', {
    label: 'Edit Whitelist', advanced: true,
    type: 'button',
    value: null,
  }, () => {
    ctx.broadcastData('openModal', {
      modal: 'editList',
      list: 'whitelist',
      title: 'Edit Whitelist',
      description: 'Items on this list are <strong>always looted</strong>, even if below your tier threshold.',
      current: readListFile('whitelist'),
      saveKey: '_saveWhitelist',
    });
  });

  ctx.registerSetting('editBlacklist', {
    label: 'Edit Blacklist', advanced: true,
    type: 'button',
    value: null,
  }, () => {
    ctx.broadcastData('openModal', {
      modal: 'editList',
      list: 'blacklist',
      title: 'Edit Blacklist',
      description: 'Items on this list are <strong>never looted</strong>, overrides the whitelist and all tier settings.',
      current: readListFile('blacklist'),
      saveKey: '_saveBlacklist',
    });
  });

  ctx.registerSetting('listHelp', {
    label: 'List Help', advanced: true,
    type: 'button',
    value: null,
  }, () => {
    ctx.broadcastData('openModal', { modal: 'listHelp', title: 'Whitelist & Blacklist Help' });
  });

  ctx.registerSetting('reloadLists', {
    label: 'Reload Lists from Disk', advanced: true,
    type: 'button',
    value: null,
  }, () => {
    reloadLists(true);
  });

  function getState(client: ClientConnection): AutoLootState {
    let state = states.get(client);
    if (!state) {
      state = {
        lastPickupAt: 0,
        recentAttempts: new Map<string, number>(),
        pendingDestSlotId: null,
        pendingDestQuantity: null,
        pendingPotionItemId: null,
        pendingSince: 0,
        bagSeenAt: new Map<number, number>(),
        notifiedBagIds: new Set<number>(),
        lastPos: null,
        stationaryTicks: 0,
        reservedDestSlots: new Map<number, number>(),
        consumedBagSlots: new Map<string, number>(),
        manualPotionSuppressUntil: 0,
        lastManualPotionSuppressLogAt: 0,
        manualPotionPacketBlockUntil: 0,
        lastManualPotionPacketBlockLogAt: 0,
      };
      states.set(client, state);
    }
    return state;
  }
  function cleanupReservations(state: AutoLootState, now: number): void {
    for (const [slot, until] of state.reservedDestSlots.entries()) {
      if (until <= now) state.reservedDestSlots.delete(slot);
    }

    for (const [key, until] of state.consumedBagSlots.entries()) {
      if (until <= now) state.consumedBagSlots.delete(key);
    }
  }

  function isReservedDestination(state: AutoLootState, packetSlotId: number, now: number): boolean {
    return Number(state.reservedDestSlots.get(packetSlotId) || 0) > now;
  }

  function makeBagSlotKey(bag: TrackedEntity, bagSlot: number, itemId: number): string {
    return `${bag.objectId}:${bagSlot}:${itemId}`;
  }

  function isQuickslotPacketSlot(packetSlotId: number): boolean {
    return packetSlotId >= QUICKSLOT_PACKET_BASE
      && packetSlotId < (QUICKSLOT_PACKET_BASE + QUICK_SLOT_COUNT);
  }

  function readQuickSlot(client: ClientConnection, slot: number): { itemType: number; quantity: number } {
    const raw = (client.playerData as any).quickSlots?.[slot];

    if (typeof raw === 'number') {
      return { itemType: raw > 0 ? raw : -1, quantity: 0 };
    }

    if (raw && typeof raw === 'object') {
      const itemTypeRaw = Number(raw.itemType ?? -1);
      const quantityRaw = Number(raw.quantity ?? 0);
      return {
        itemType: itemTypeRaw > 0 ? itemTypeRaw : -1,
        quantity: Number.isFinite(quantityRaw) ? Math.max(0, Math.trunc(quantityRaw)) : 0,
      };
    }

    return { itemType: -1, quantity: 0 };
  }

  function isPotionItem(itemId: number): boolean {
    return HP_POTION_IDS.has(itemId)
      || MP_POTION_IDS.has(itemId)
      || STAT_POTION_IDS.has(itemId)
      || LIFE_MANA_POTION_IDS.has(itemId);
  }

  function isHpOrMpPotion(itemId: number): boolean {
    return HP_POTION_IDS.has(itemId) || MP_POTION_IDS.has(itemId);
  }

  function packetTouchesQuickslotOrPotion(packet: any): boolean {
    const data = packet?.data ?? {};
    const slotObjects = [data.slotObject1, data.slotObject2, data.slotObject].filter(Boolean);

    for (const slotObject of slotObjects) {
      const slotId = Number(slotObject?.slotId ?? -1);
      const objectType = Number(slotObject?.objectType ?? -1);

      if (Number.isFinite(slotId) && isQuickslotPacketSlot(slotId)) return true;
      if (Number.isFinite(objectType) && isPotionItem(objectType)) return true;
    }

    return false;
  }

  function suppressHpMpPotionLootAfterManualAction(
    client: ClientConnection,
    reason: string,
    clearPendingAutoLoot = true,
  ): void {
    const state = getState(client);
    const now = Date.now();
    const pauseMs = Math.max(
      MIN_MANUAL_POTION_SUPPRESS_MS,
      Math.min(MAX_MANUAL_POTION_SUPPRESS_MS, Math.trunc(Number(manualPotionSuppressMs) || DEFAULT_MANUAL_POTION_SUPPRESS_MS)),
    );

    state.manualPotionSuppressUntil = Math.max(state.manualPotionSuppressUntil, now + pauseMs);

    if (clearPendingAutoLoot) {
      state.pendingDestSlotId = null;
      state.pendingDestQuantity = null;
      state.pendingPotionItemId = null;
      state.pendingSince = 0;
    }

    state.recentAttempts.clear();
    state.reservedDestSlots.clear();
    state.consumedBagSlots.clear();

    if ((now - Number(state.lastManualPotionSuppressLogAt || 0)) >= MANUAL_POTION_SUPPRESS_LOG_MS) {
      state.lastManualPotionSuppressLogAt = now;
      ctx.log(`[Manual Potion Guard] Pausing Auto Loot for ${pauseMs}ms after ${reason}`);
    }
  }

  function isPendingHpMpAutoLootActive(state: AutoLootState, now: number): boolean {
    return state.pendingPotionItemId != null
      && state.pendingDestSlotId != null
      && (now - Number(state.pendingSince || 0)) < PENDING_DEST_TIMEOUT_MS;
  }
  function getManualPotionPacketBlockMs(): number {
    return Math.max(
      MIN_MANUAL_POTION_PACKET_BLOCK_MS,
      Math.min(
        MAX_MANUAL_POTION_PACKET_BLOCK_MS,
        Math.trunc(Number(manualPotionPacketBlockMs) || DEFAULT_MANUAL_POTION_PACKET_BLOCK_MS),
      ),
    );
  }
    function blockManualPotionPacketDuringPendingAutoLoot(
    client: ClientConnection,
    packetName: string,
  ): void {
    const state = getState(client);
    const now = Date.now();

    // Important:
    // Do NOT extend manualPotionPacketBlockUntil here.
    // The deadline is set once when Auto Loot sends the HP/MP swap.
    // Extending it on every blocked manual packet can desync the client during spam.
    const blockMs = getManualPotionPacketBlockMs();
    const remainingMs = Math.max(
      0,
      Math.ceil(Number(state.manualPotionPacketBlockUntil || 0) - now),
    );

    suppressHpMpPotionLootAfterManualAction(client, packetName, false);

    if ((now - Number(state.lastManualPotionPacketBlockLogAt || 0)) >= MANUAL_POTION_PACKET_BLOCK_LOG_MS) {
      state.lastManualPotionPacketBlockLogAt = now;
      ctx.log(
        `[Manual Potion Guard] Blocked ${packetName} for ${remainingMs > 0 ? remainingMs : blockMs}ms ` +
        `while pending Auto Loot potion swap settles`,
      );
    }
  }

  function getPlayerSlotObjectType(client: ClientConnection, packetSlotId: number): number {
    if (packetSlotId >= 0 && packetSlotId <= 11) {
      return Number(client.playerData.inventory[packetSlotId] ?? -1);
    }
    if (packetSlotId >= 12 && packetSlotId <= 27) {
      return Number(client.playerData.backpack[packetSlotId - 12] ?? -1);
    }
    if (isQuickslotPacketSlot(packetSlotId)) {
      return readQuickSlot(client, packetSlotId - QUICKSLOT_PACKET_BASE).itemType;
    }
    return -1;
  }

  function getQuickslotDestination(client: ClientConnection, itemId: number): LootDestination | null {
    const info = catalog.items.get(itemId);
    if (info && info.quickslotAllowed !== true) return null;

    if (!HP_POTION_IDS.has(itemId) && !MP_POTION_IDS.has(itemId)) return null;

    const maxStack = HP_POTION_IDS.has(itemId)
      ? MAX_HP_QUICKSLOT_STACK
      : MAX_MP_QUICKSLOT_STACK;

    let existingSlot = -1;
    let existingQuantity = 0;
    let firstEmptySlot = -1;

    const quickSlotCount = client.playerData.hasThirdQuickSlot ? 3 : 2;
    for (let slot = 0; slot < quickSlotCount; slot++) {
      const current = readQuickSlot(client, slot);

      if (current.itemType === itemId) {
        existingSlot = slot;
        existingQuantity = current.quantity;
        break;
      }

      if (current.itemType === -1 && firstEmptySlot < 0) {
        firstEmptySlot = slot;
      }
    }

    if (existingSlot >= 0) {
      // Stack only when the real per-slot stackCount is known and below cap.
      if (existingQuantity > 0 && existingQuantity < maxStack) {
        return {
          packetSlotId: QUICKSLOT_PACKET_BASE + existingSlot,
          currentObjectType: itemId,
        };
      }

      // Existing HP/MP quickslot is full or quantity is unknown. Do not create
      // duplicate HP/MP stacks in other empty quickslots.
      return null;
    }

    if (firstEmptySlot >= 0) {
      return {
        packetSlotId: QUICKSLOT_PACKET_BASE + firstEmptySlot,
        currentObjectType: -1,
      };
    }

    return null;
  }

  function getExpectedQuickslotQuantity(client: ClientConnection, destination: LootDestination, itemId: number): number | null {
    if (!isQuickslotPacketSlot(destination.packetSlotId)) return null;
    if (!HP_POTION_IDS.has(itemId) && !MP_POTION_IDS.has(itemId)) return null;

    const quickslotIndex = destination.packetSlotId - QUICKSLOT_PACKET_BASE;
    const current = readQuickSlot(client, quickslotIndex);
    const maxStack = HP_POTION_IDS.has(itemId)
      ? MAX_HP_QUICKSLOT_STACK
      : MAX_MP_QUICKSLOT_STACK;

    if (current.itemType !== itemId || current.quantity <= 0) return 1;
    return Math.min(maxStack, current.quantity + 1);
  }

  function getFirstFreeLootDestination(
    client: ClientConnection,
    allowBackpack: boolean,
    backpackFirst: boolean,
    state: AutoLootState,
    now: number,
  ): LootDestination | null {
    const tryInventory = (): LootDestination | null => {
      for (let slot = 4; slot <= 11; slot++) {
        const objectType = Number(client.playerData.inventory[slot] ?? -1);
        if (objectType !== -1) continue;
        if (isReservedDestination(state, slot, now)) continue;
        return { packetSlotId: slot, currentObjectType: -1 };
      }
      return null;
    };

    const tryBackpack = (): LootDestination | null => {
      if (!allowBackpack || !client.playerData.hasBackpack) return null;
      // backpackTier 8 = 8 slots (no extender), 16 = 16 slots (with extender)
      const backpackSize = client.playerData.hasBackpackExtender ? 16 : 8;
      for (let slot = 0; slot < backpackSize; slot++) {
        const packetSlotId = 12 + slot;
        const objectType = Number(client.playerData.backpack[slot] ?? -1);
        if (objectType !== -1) continue;
        if (isReservedDestination(state, packetSlotId, now)) continue;
        return { packetSlotId, currentObjectType: -1 };
      }
      return null;
    };

    if (backpackFirst) {
      return tryBackpack() ?? tryInventory();
    }
    return tryInventory() ?? tryBackpack();
  }

  function getBagItemId(entity: TrackedEntity, bagSlot: number): number {
    const value = entity.stats?.[String(StatType.Inventory0 + bagSlot)];
    const itemId = Number(value);
    return Number.isFinite(itemId) ? Math.trunc(itemId) : -1;
  }

  function getBagDisplayName(objectType: number): string {
    const worldName = ctx.gameData?.getObject(objectType)?.id?.trim();
    if (worldName) return worldName;
    return `0x${objectType.toString(16)}`;
  }

  function getItemDisplayName(itemId: number): string {
    const catalogName = catalog.items.get(itemId)?.name?.trim();
    if (catalogName) return catalogName;
    const worldName = ctx.gameData?.getObject(itemId)?.id?.trim();
    if (worldName) return worldName;
    return `0x${itemId.toString(16)}`;
  }

  function formatBagContents(entity: TrackedEntity): string {
    const names: string[] = [];
    for (let bagSlot = 0; bagSlot < 8; bagSlot++) {
      const itemId = getBagItemId(entity, bagSlot);
      if (!Number.isFinite(itemId) || itemId <= 0) continue;
      const label = getItemDisplayName(itemId);
      const enchantIds = getBagSlotEnchantIds(entity.stats, bagSlot);
      names.push(
        enchantIds.length > 0 ? `${label} (${enchantIds.join(', ')})` : label,
      );
    }

    if (names.length === 0) return 'empty';
    if (names.length <= BAG_NOTIFY_ITEM_LIMIT) return names.join(', ');
    return `${names.slice(0, BAG_NOTIFY_ITEM_LIMIT).join(', ')} (+${names.length - BAG_NOTIFY_ITEM_LIMIT} more)`;
  }

  function getTrackedBags(client: ClientConnection, radius?: number): TrackedEntity[] {
    if (!ctx.worldState) return [];
    const playerPos = client.playerData.pos;
    if (!playerPos) return [];

    return ctx.worldState
      .getEntitiesInRadius(playerPos, radius ?? Number.MAX_SAFE_INTEGER)
      .filter((entity) => BAG_TYPES.has(entity.objectType))
      .sort((a, b) => {
        const da = Math.hypot(Number(a.pos?.x || 0) - playerPos.x, Number(a.pos?.y || 0) - playerPos.y);
        const db = Math.hypot(Number(b.pos?.x || 0) - playerPos.x, Number(b.pos?.y || 0) - playerPos.y);
        return da - db;
      });
  }

  function getGearCategory(slotType: number): GearCategory | null {
    if (WEAPON_SLOT_TYPES.has(slotType)) return 'weapon';
    if (ABILITY_SLOT_TYPES.has(slotType)) return 'ability';
    if (ARMOR_SLOT_TYPES.has(slotType)) return 'armor';
    if (RING_SLOT_TYPES.has(slotType)) return 'ring';
    return null;
  }

  function getMinimumTierForCategory(category: GearCategory): number {
    if (category === 'weapon') return minWeaponTier;
    if (category === 'ability') return minAbilityTier;
    if (category === 'armor') return minArmorTier;
    return minRingTier;
  }

  function shouldSkipMap(mapName: string): boolean {
    const lower = mapName.trim().toLowerCase();
    if (!lower) return false;
    return lower.includes('vault') || lower === 'daily quest room' || lower.startsWith('pet yard');
  }

  function isAnyPotionItem(itemId: number): boolean {
    return HP_POTION_IDS.has(itemId)
      || MP_POTION_IDS.has(itemId)
      || LIFE_MANA_POTION_IDS.has(itemId)
      || STAT_POTION_IDS.has(itemId);
  }

  /** Stat pot on ground: interact if looting and/or autodrinking, or whitelisted. */
  function canInteractWithStatPotOnBag(itemId: number): boolean {
    if (!STAT_POTION_IDS.has(itemId)) return false;
    if (blacklist.has(itemId)) return false;
    if (whitelist.has(itemId)) return true;
    return lootStatPotions || autodrinkStatPots;
  }

  /**
   * After `shouldLootItem` is true: require enough decoded enchant IDs on this bag slot
   * when min tier is set. Whitelist and all potion types skip this gate.
   */
  function passesMinEnchantGate(itemId: number, bag: TrackedEntity, bagSlot: number): boolean {
    if (whitelist.has(itemId)) return true;
    if (isAnyPotionItem(itemId)) return true;
    if (minEnchantTier <= 0) return true;
    const count = getBagSlotEnchantIds(bag.stats, bagSlot).length;
    return count >= minEnchantTier;
  }

  function shouldLootItem(itemId: number): boolean {
    if (!Number.isFinite(itemId) || itemId <= 0) return false;
    if (blacklist.has(itemId)) return false;
    if (whitelist.has(itemId)) return true;
    if (HP_POTION_IDS.has(itemId)) return lootHpPotions;
    if (MP_POTION_IDS.has(itemId)) return lootMpPotions;
    if (LIFE_MANA_POTION_IDS.has(itemId)) return lootLifeManaPotions;
    if (STAT_POTION_IDS.has(itemId)) return lootStatPotions;

    const info = catalog.items.get(itemId);
    if (!info) {
      const rawObj = ctx.gameData?.getObject(itemId);
      if (rawObj && lootUTs) {
        const st = Math.trunc(Number(rawObj.slotType ?? -1));
        const tierU = String(rawObj.tierStr ?? '').trim().toUpperCase();
        if (isMultitoolUtTier(tierU, st) && st !== 10 && st !== 26) return true;
      }
      // DEBUG: item present in raw game data but missing from Auto Loot catalog.
      // if (rawObj) {
      //   const tier = String(rawObj.tierStr ?? '').trim().toUpperCase();
      //   ctx.dashboardLog(`[DEBUG] 0x${itemId.toString(16)} "${rawObj.id}" not in catalog tierStr="${tier}" slotType=${rawObj.slotType}`);
      // }
      return false;
    }

    if (lootMarks && info.name.includes('Mark of ')) return true;
    if (lootEggs && info.name.endsWith(' Egg')) return true;

    if (info.isUT) {
      if (!lootUTs) {
        // DEBUG:
        // ctx.log(`[Auto Loot UT DEBUG] rejected="${info.name}" id=${itemId} reason=lootUTsDisabled`);
        return false;
      }

      const result = info.slotType !== 10 && info.slotType !== 26;

      // DEBUG:
      // if (!result) {
      //   ctx.log(`[Auto Loot UT DEBUG] rejected="${info.name}" id=${itemId} reason=excludedSlotType slotType=${info.slotType}`);
      // } else {
      //   ctx.log(`[Auto Loot UT DEBUG] accepted="${info.name}" id=${itemId} slotType=${info.slotType}`);
      // }

      return result;
    }

    if (info.isST) {
      return lootSTs;
    }

    const category = getGearCategory(info.slotType);
    if (!category) return false;
    return info.tier != null && info.tier >= getMinimumTierForCategory(category);
  }

  function updateIdleState(client: ClientConnection, state: AutoLootState): void {
    const pos = client.playerData.pos;
    if (!pos || !Number.isFinite(pos.x) || !Number.isFinite(pos.y)) return;

    if (!state.lastPos) {
      state.lastPos = { x: pos.x, y: pos.y };
      state.stationaryTicks = 0;
      return;
    }

    const moved = Math.abs(pos.x - state.lastPos.x) > MOVEMENT_EPSILON || Math.abs(pos.y - state.lastPos.y) > MOVEMENT_EPSILON;
    if (moved) {
      state.lastPos = { x: pos.x, y: pos.y };
      state.stationaryTicks = 0;
      return;
    }

    state.stationaryTicks += 1;
  }

  function getNearbyBags(client: ClientConnection, state: AutoLootState): TrackedEntity[] {
    const found = new Map<number, TrackedEntity>();
    const now = Date.now();
    const worldState = ctx.worldState;

    for (const entity of getTrackedBags(client, ON_TOP_DISTANCE + 0.1)) {
      found.set(entity.objectId, entity);
      if (!state.bagSeenAt.has(entity.objectId)) {
        state.bagSeenAt.set(entity.objectId, now);
      }
    }

    for (const objectId of [...state.bagSeenAt.keys()]) {
      if (!found.has(objectId) && !worldState?.getEntity(objectId)) {
        state.bagSeenAt.delete(objectId);
      }
    }

    return [...found.values()];
  }

  function notifyNewBags(client: ClientConnection, state: AutoLootState): void {
    if (!bagNotifierEnabled || !ctx.worldState) return;
    const playerPos = client.playerData.pos;
    if (!playerPos) return;

    const visibleBags = getTrackedBags(client, BAG_NOTIFY_RADIUS);
    for (const objectId of [...state.notifiedBagIds]) {
      if (!ctx.worldState.getEntity(objectId)) {
        state.notifiedBagIds.delete(objectId);
      }
    }

    for (const bag of visibleBags) {
      if (state.notifiedBagIds.has(bag.objectId)) continue;
      const distance = Math.hypot(Number(bag.pos?.x || 0) - playerPos.x, Number(bag.pos?.y || 0) - playerPos.y);
      const bagName = getBagDisplayName(bag.objectType);
      const contents = formatBagContents(bag);
      ctx.sendNotification(
        client,
        'Auto Loot',
        `Bag appeared (${bagName}, ${distance.toFixed(1)}t): ${contents}`,
      );
      state.notifiedBagIds.add(bag.objectId);
    }
  }

  function shouldDelayPublicBag(bag: TrackedEntity, state: AutoLootState, now: number): boolean {
    if (!publicDelay || !PUBLIC_BAG_TYPES.has(bag.objectType)) return false;
    const seenAt = Number(state.bagSeenAt.get(bag.objectId) || 0);
    if (!seenAt) return false;
    return (now - seenAt) < PUBLIC_BAG_DELAY_MS;
  }

  function sendUseItemFromBag(
    client: ClientConnection,
    bag: TrackedEntity,
    bagSlot: number,
    itemId: number,
  ): void {
    const packet = ctx.createPacket('USEITEM');
    packet.data.time = Math.trunc(client.time);
    packet.data.slotObject = {
      objectId: bag.objectId,
      slotId: bagSlot,
      objectType: itemId,
    };
    packet.data.itemUsePos = { x: 0, y: 0 };
    packet.data.useType = 0;
    packet.data.unknownInt = 0;
    packet.modified = true;
    client.sendToServer(packet);
  }

  function sendLootSwap(
    client: ClientConnection,
    bag: TrackedEntity,
    bagSlot: number,
    itemId: number,
    destination: LootDestination,
  ): boolean {
    const state = getState(client);
    const now = Date.now();

    // Safety: after a manual potion/quickslot action, pause all Auto Loot swaps.
    // Even non-potion swaps can collide with unsettled inventory state if the
    // player just moved/dropped stacked quickslot potions.
    if (now < Number(state.manualPotionSuppressUntil || 0)) {
      return false;
    }

    const packet = ctx.createPacket('INVENTORYSWAP');
    packet.data.time = Math.trunc(client.time);
    packet.data.position = {
      x: Number(client.playerData.pos?.x ?? 0),
      y: Number(client.playerData.pos?.y ?? 0),
    };
    packet.data.slotObject1 = {
      objectId: bag.objectId,
      slotId: bagSlot,
      objectType: itemId,
    };
    packet.data.slotObject2 = {
      objectId: client.objectId,
      slotId: destination.packetSlotId,
      objectType: destination.currentObjectType,
    };
    packet.modified = true;
    client.sendToServer(packet);
    return true;
  }

  function sendPlayerSwap(
    client: ClientConnection,
    fromPacketSlotId: number,
    itemId: number,
    destination: LootDestination,
  ): void {
    const packet = ctx.createPacket('INVENTORYSWAP');
    packet.data.time = Math.trunc(client.time);
    packet.data.position = {
      x: Number(client.playerData.pos?.x ?? 0),
      y: Number(client.playerData.pos?.y ?? 0),
    };
    packet.data.slotObject1 = {
      objectId: client.objectId,
      slotId: fromPacketSlotId,
      objectType: itemId,
    };
    packet.data.slotObject2 = {
      objectId: client.objectId,
      slotId: destination.packetSlotId,
      objectType: destination.currentObjectType,
    };
    packet.modified = true;
    client.sendToServer(packet);
  }

  function findQuickslotRestockMove(_client: ClientConnection): { fromPacketSlotId: number; itemId: number; destination: LootDestination } | null {
    // SAFETY: player inventory/backpack -> quickslot INVENTORYSWAP can desync
    // while the player is manually dragging/dropping stacked potions. Bag/ground
    // -> quickslot autoloot is handled separately and remains enabled.
    return null;
  }

  // ─── Diagnostic logging ───────────────────────────────────────────────────────

  let diagEnabled = false;
  let _diagLastPeriodicMs = 0;

  function diagLog(msg: string): void {
    if (diagEnabled) ctx.log(`[DIAG] ${msg}`);
  }

  function diagInventorySnapshot(client: ClientConnection): string {
    const inv = client.playerData.inventory ?? [];
    const bp  = client.playerData.backpack  ?? [];
    const qs  = (client.playerData as any).quickSlots ?? [];
    const freeInv = [4,5,6,7,8,9,10,11].filter((s) => Number(inv[s] ?? -1) === -1).length;
    const bpSize  = client.playerData.hasBackpack ? (client.playerData.hasBackpackExtender ? 16 : 8) : 0;
    const freeBp  = bpSize > 0
      ? Array.from({ length: bpSize }, (_, i) => Number(bp[i] ?? -1)).filter((v) => v === -1).length
      : -1;
    const qsCount = client.playerData.hasThirdQuickSlot ? 3 : 2;
    const qsParts = Array.from({ length: qsCount }, (_, i) => {
      const s: any = qs[i];
      if (s && typeof s === 'object') return `qs${i}=${s.itemType}x${s.quantity}`;
      return `qs${i}=${typeof s === 'number' ? s : -1}`;
    });
    return `inv_free=${freeInv} bp_free=${freeBp === -1 ? 'n/a' : freeBp}(size=${bpSize}) qs_count=${qsCount} ${qsParts.join(' ')}`;
  }

  ctx.registerSetting('diagEnabled', {
    label: 'Diagnostic Logging', advanced: true,
    type: 'boolean',
    value: false,
  }, (value: boolean) => {
    diagEnabled = value === true;
    ctx.log(`Auto Loot diagnostic logging: ${diagEnabled ? 'ON' : 'OFF'}`);
  });

  function tryAutoLoot(client: ClientConnection): void {
    if (!ctx.enabled || !ctx.worldState) return;
    if (!client?.connected || !client.objectId) return;

    const mapName = client.playerData.mapName || '';
    if (shouldSkipMap(mapName)) {
      diagLog(`tryAutoLoot skip — map="${mapName}"`);
      return;
    }

    const playerPos = client.playerData.pos;
    if (!playerPos || !Number.isFinite(playerPos.x) || !Number.isFinite(playerPos.y)) return;

    const state = getState(client);
    const now = Date.now();

    notifyNewBags(client, state);
    updateIdleState(client, state);

    if (disableWhenIdle && state.stationaryTicks > STATIONARY_TICK_LIMIT) {
      diagLog(`tryAutoLoot skip — idle (ticks=${state.stationaryTicks})`);
      return;
    }

    if (state.pendingDestSlotId != null) {
      if (isQuickslotPacketSlot(state.pendingDestSlotId) && state.pendingDestQuantity != null) {
        const quickslotIndex = state.pendingDestSlotId - QUICKSLOT_PACKET_BASE;
        const current = readQuickSlot(client, quickslotIndex);
        const timedOut = (now - state.pendingSince) >= PENDING_DEST_TIMEOUT_MS;
        diagLog(`pending qs slot=${state.pendingDestSlotId} curQty=${current.quantity} wantQty=${state.pendingDestQuantity} timedOut=${timedOut}`);
        if (current.quantity >= state.pendingDestQuantity || timedOut) {
          state.pendingDestSlotId = null;
          state.pendingDestQuantity = null;
          state.pendingPotionItemId = null;
          state.pendingSince = 0;
        }
      } else {
        const current = getPlayerSlotObjectType(client, state.pendingDestSlotId);
        const timedOut = (now - state.pendingSince) >= PENDING_DEST_TIMEOUT_MS;
        diagLog(`pending inv slot=${state.pendingDestSlotId} curType=${current} timedOut=${timedOut}`);
        if (current !== -1 || timedOut) {
          state.pendingDestSlotId = null;
          state.pendingDestQuantity = null;
          state.pendingPotionItemId = null;
          state.pendingSince = 0;
        }
      }
    }
    if (state.pendingDestSlotId != null) {
      diagLog(`tryAutoLoot skip — pendingDestSlot=${state.pendingDestSlotId}`);
      return;
    }
    if ((now - state.lastPickupAt) < PICKUP_INTERVAL_MS) return;

    for (const [key, attemptedAt] of state.recentAttempts.entries()) {
      if ((now - attemptedAt) >= RETRY_ITEM_AFTER_MS) {
        state.recentAttempts.delete(key);
      }
    }
    cleanupReservations(state, now);

    if (restockQuickSlots) {
      const restock = findQuickslotRestockMove(client);
      if (restock) {
        const attemptKey = `restock:${restock.fromPacketSlotId}:${restock.itemId}:${restock.destination.packetSlotId}`;
        const lastAttempt = Number(state.recentAttempts.get(attemptKey) || 0);
        if ((now - lastAttempt) >= RETRY_ITEM_AFTER_MS) {
          const expectedQuantity = getExpectedQuickslotQuantity(client, restock.destination, restock.itemId);
          diagLog(`SEND restock INVENTORYSWAP item=${restock.itemId} from=${restock.fromPacketSlotId} to=${restock.destination.packetSlotId}`);
          sendPlayerSwap(client, restock.fromPacketSlotId, restock.itemId, restock.destination);
          state.lastPickupAt = now;
          state.pendingDestSlotId = restock.destination.packetSlotId;
          state.pendingDestQuantity = expectedQuantity;
          state.pendingPotionItemId = isHpOrMpPotion(restock.itemId) ? restock.itemId : null;
          state.pendingSince = now;
          state.recentAttempts.set(attemptKey, now);
          return;
        }
      }
    }

    const generalDestination = getFirstFreeLootDestination(client, useBackpack, preferBackpack, state, now);

    const bags = getNearbyBags(client, state);

    // Periodic diagnostic: log current state whenever bags are nearby
    if (diagEnabled && bags.length > 0 && (now - _diagLastPeriodicMs) >= 2000) {
      _diagLastPeriodicMs = now;
      const invSnap = diagInventorySnapshot(client);
      const bagSummary = bags.map((b) => {
        const dist = Math.hypot(Number(b.pos?.x || 0) - playerPos.x, Number(b.pos?.y || 0) - playerPos.y);
        const items: string[] = [];
        for (let s = 0; s < 8; s++) {
          const id = getBagItemId(b, s);
          if (id > 0) items.push(`${id}(${getItemDisplayName(id)})`);
        }
        return `bag#${b.objectId}(type=${b.objectType},dist=${dist.toFixed(2)})=[${items.join(',')}]`;
      }).join(' ');
      ctx.log(`[DIAG] map="${mapName}" pos=(${playerPos.x.toFixed(1)},${playerPos.y.toFixed(1)}) ${invSnap} genDest=${generalDestination ? generalDestination.packetSlotId : 'null(full)'} manualSuppress=${Math.max(0, Number(state.manualPotionSuppressUntil || 0) - now)}ms`);
      ctx.log(`[DIAG] nearbyBags(${bags.length}): ${bagSummary}`);
    }

    for (const bag of bags) {
      const distance = Math.hypot(Number(bag.pos?.x || 0) - playerPos.x, Number(bag.pos?.y || 0) - playerPos.y);
      if (!Number.isFinite(distance) || distance > ON_TOP_DISTANCE) continue;
      if (shouldDelayPublicBag(bag, state, now)) {
        diagLog(`bag#${bag.objectId} skip — public delay`);
        continue;
      }

      for (let bagSlot = 0; bagSlot < 8; bagSlot++) {
        const itemId = getBagItemId(bag, bagSlot);
        if (itemId <= 0) continue;
        const bagSlotKey = makeBagSlotKey(bag, bagSlot, itemId);

        if (now < Number(state.manualPotionSuppressUntil || 0)) {
          diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId} skip — manualSuppress ${Math.ceil(Number(state.manualPotionSuppressUntil) - now)}ms`);
          continue;
        }

        if (Number(state.consumedBagSlots.get(bagSlotKey) || 0) > now) {
          diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId} skip — consumedBagSlot`);
          continue;
        }

        const isStat = STAT_POTION_IDS.has(itemId);
        if (isStat) {
          if (!canInteractWithStatPotOnBag(itemId)) {
            diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId}(${getItemDisplayName(itemId)}) skip — statPot not interactable (lootStat=${lootStatPotions} autodrink=${autodrinkStatPots} blacklist=${blacklist.has(itemId)})`);
            continue;
          }
        } else if (!shouldLootItem(itemId)) {
          diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId}(${getItemDisplayName(itemId)}) skip — shouldLootItem=false`);
          continue;
        }
        if (!passesMinEnchantGate(itemId, bag, bagSlot)) {
          diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId} skip — enchant gate`);
          continue;
        }

        if (autodrinkStatPots && isStat) {
          if (
            ctx.gameData
            && shouldSkipAutodrinkClassCap(
              client.playerData.classType,
              client.playerData,
              itemId,
              (ct) => ctx.gameData!.getPlayerClassStatMaxes(ct),
            )
          ) {
            diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId} skip — autodrink class cap`);
            continue;
          }
          const attemptKey = `drink:${bag.objectId}:${bagSlot}:${itemId}`;
          const lastAttempt = Number(state.recentAttempts.get(attemptKey) || 0);
          if ((now - lastAttempt) < RETRY_ITEM_AFTER_MS) {
            diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId} skip — autodrink cooldown ${RETRY_ITEM_AFTER_MS - (now - lastAttempt)}ms`);
            continue;
          }

          ctx.log(`[DIAG] SEND autodrink USEITEM bag#${bag.objectId} slot=${bagSlot} item=${itemId}(${getItemDisplayName(itemId)}) ${diagInventorySnapshot(client)}`);
          sendUseItemFromBag(client, bag, bagSlot, itemId);
          state.lastPickupAt = now;
          state.recentAttempts.set(attemptKey, now);
          return;
        }

        const destination = getQuickslotDestination(client, itemId) ?? generalDestination;

        diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId}(${getItemDisplayName(itemId)}) dest=${destination ? destination.packetSlotId : 'null'} genDest=${generalDestination ? generalDestination.packetSlotId : 'null(full)'}`);

        if (!destination) continue;

        const attemptKey = `${bag.objectId}:${bagSlot}:${itemId}`;
        const lastAttempt = Number(state.recentAttempts.get(attemptKey) || 0);
        if ((now - lastAttempt) < RETRY_ITEM_AFTER_MS) {
          diagLog(`bag#${bag.objectId} slot=${bagSlot} item=${itemId} skip — retry cooldown ${RETRY_ITEM_AFTER_MS - (now - lastAttempt)}ms`);
          continue;
        }

        const expectedQuantity = getExpectedQuickslotQuantity(client, destination, itemId);
        ctx.log(`[DIAG] SEND INVENTORYSWAP bag#${bag.objectId} slot=${bagSlot} item=${itemId}(${getItemDisplayName(itemId)}) → destSlot=${destination.packetSlotId} ${diagInventorySnapshot(client)}`);
        const sent = sendLootSwap(client, bag, bagSlot, itemId, destination);
        if (!sent) {
          ctx.log(`[DIAG] sendLootSwap returned false (manualSuppress active)`);
          continue;
        }

        state.lastPickupAt = now;
        state.pendingDestSlotId = destination.packetSlotId;
        state.pendingDestQuantity = expectedQuantity;
        state.pendingPotionItemId = isHpOrMpPotion(itemId) ? itemId : null;
        state.pendingSince = now;

        if (isHpOrMpPotion(itemId)) {
          const blockMs = getManualPotionPacketBlockMs();
          state.manualPotionPacketBlockUntil = Math.max(
            Number(state.manualPotionPacketBlockUntil || 0),
            now + blockMs,
          );
          diagLog(`manualPotionBlock set for ${blockMs}ms`);
        }

        state.recentAttempts.set(attemptKey, now);
        const shouldReserveForPotionSafety = isHpOrMpPotion(itemId) || isQuickslotPacketSlot(destination.packetSlotId);
        if (shouldReserveForPotionSafety) {
          state.reservedDestSlots.set(destination.packetSlotId, now + DEST_SLOT_RESERVE_MS);
          state.consumedBagSlots.set(bagSlotKey, now + BAG_SLOT_CONSUME_MS);
        }
        return;
      }
    }
    // Nothing was sent this pass — rate-limit re-evaluation to avoid re-scanning every tick.
    state.lastPickupAt = now;
  }

  function resetState(client: ClientConnection): void {
    const state = getState(client);
    state.lastPickupAt = 0;
    state.recentAttempts.clear();
    state.pendingDestSlotId = null;
    state.pendingDestQuantity = null;
    state.pendingPotionItemId = null;
    state.pendingSince = 0;
    state.bagSeenAt.clear();
    state.notifiedBagIds.clear();
    state.lastPos = null;
    state.stationaryTicks = 0;
    state.reservedDestSlots.clear();
    state.consumedBagSlots.clear();
    state.manualPotionSuppressUntil = 0;
    state.lastManualPotionSuppressLogAt = 0;
    state.manualPotionPacketBlockUntil = 0;
    state.lastManualPotionPacketBlockLogAt = 0;
  }

  /**
      * Multitool `Class88.method_2` (incoming `UPDATE` -> `newObjs`): for each `Bags` object type,
   * set numeric `Size` (stat 2) to 175. Only mutates existing Size entries; does not add stats.
   */
  function rewriteBigBagSizeMultitool(
    status: { data?: Array<{ id: unknown; value: unknown }> } | undefined,
  ): boolean {
    if (!status?.data || !Array.isArray(status.data)) return false;
    let changed = false;
    for (const s of status.data) {
      if (toStatInt(s.id) !== StatType.Size) continue;
      if (typeof s.value === 'string') continue;
      if (toStatInt(s.value) === MULTITOOL_BIG_BAG_SIZE) continue;
      s.value = MULTITOOL_BIG_BAG_SIZE;
      changed = true;
    }
    return changed;
  }

  ctx.hookPacket('UPDATE', (_client, packet) => {
    if (!bigLootBags) return;
    if (!packet.isDefined) return;
    const newObjs = packet.data.newObjs as Array<{
      objectType?: number;
      status?: { data?: Array<{ id: unknown; value: unknown }> };
    }> | undefined;
    if (!Array.isArray(newObjs) || newObjs.length === 0) return;

    let changed = false;
    for (const obj of newObjs) {
      const ot = toStatInt(obj.objectType);
      if (!BAG_TYPES.has(ot) || !obj.status) continue;
      if (rewriteBigBagSizeMultitool(obj.status)) changed = true;
    }
    if (changed) packet.modified = true;
  });

  ctx.hookAllPackets((client, packet, fromClient) => {
    if (!fromClient) return;

    if (
      packet.name !== 'INVENTORYSWAP' &&
      packet.name !== 'INVDROP' &&
      packet.name !== 'USEITEM'
    ) {
      return;
    }

    if (!packetTouchesQuickslotOrPotion(packet)) return;

    const state = getState(client);
    const now = Date.now();
    const pendingHpMpAutoLoot = isPendingHpMpAutoLootActive(state, now);
    const manualPacketBlockActive = now < Number(state.manualPotionPacketBlockUntil || 0);

    ctx.log(`[DIAG] hookAllPackets intercept: ${packet.name} map="${client.playerData.mapName}" pendingHpMpAutoLoot=${pendingHpMpAutoLoot} manualBlockActive=${manualPacketBlockActive} data=${JSON.stringify(packet.data)}`);

    if (pendingHpMpAutoLoot || manualPacketBlockActive) {
      packet.send = false;
      ctx.log(`[DIAG] hookAllPackets BLOCKED ${packet.name} pendingHpMp=${pendingHpMpAutoLoot} blockActive=${manualPacketBlockActive}`);
      blockManualPotionPacketDuringPendingAutoLoot(client, packet.name);
      return;
    }

    suppressHpMpPotionLootAfterManualAction(client, packet.name, true);
    ctx.log(`[DIAG] hookAllPackets ALLOWED ${packet.name} — suppressing potionLoot for ${manualPotionSuppressMs}ms`);
  });

  ctx.hookPacket('NEWTICK', (client) => {
    tryAutoLoot(client);
  });

  ctx.hookPacket('MAPINFO', (client) => {
    resetState(client);
  });

  ctx.on('clientConnected', (client) => {
    resetState(client);
  });

  ctx.log(`Loaded ${catalog.items.size} lootable item defs across ${BAG_TYPES.size} bag types.`);
}
