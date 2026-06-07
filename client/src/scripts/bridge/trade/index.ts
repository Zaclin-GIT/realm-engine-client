import { trade } from '@realmengine/sdk';
import type { TradeItem } from '@realmengine/sdk';
import type { BridgeDeps } from '../BridgeDeps.js';
import type { ClientConnection } from '../../../proxy/ClientConnection.js';
import type { Packet } from '../../../packets/Packet.js';
import { Logger } from '../../../util/Logger.js';
import { normalizeSlotCount, toBoolArray, extractTradeItemIncluded } from '../../../util/tradeSlots.js';

type TradeSession = {
  active: boolean;
  ourSlotCount: number;
  partnerSlotCount: number;
  ourItems: TradeItem[];
  partnerItems: TradeItem[];
  ourOffer: boolean[];
  partnerOffer: boolean[];
  /** Partner's slot selection as last sent in S→C TRADECHANGED — must be echoed in C→S ACCEPTTRADE (see deposit-bot). */
  partnerOfferFromTradeChanged: boolean[];
  partnerName: string;
};

const sessions = new WeakMap<ClientConnection, TradeSession>();
let hooksInstalled = false;

function createSession(): TradeSession {
  return {
    active: false,
    ourSlotCount: 12,
    partnerSlotCount: 12,
    ourItems: [],
    partnerItems: [],
    ourOffer: [],
    partnerOffer: [],
    partnerOfferFromTradeChanged: [],
    partnerName: '',
  };
}

function getSession(client: ClientConnection): TradeSession {
  let session = sessions.get(client);
  if (!session) {
    session = createSession();
    sessions.set(client, session);
  }
  return session;
}

function resetSession(client: ClientConnection): void {
  sessions.set(client, createSession());
}

function cloneTradeItem(item: TradeItem): TradeItem {
  return {
    item: item.item,
    slotType: item.slotType,
    tradeable: item.tradeable,
    included: item.included,
    enchantment: item.enchantment,
  };
}

function normalizeTradeItem(raw: unknown): TradeItem {
  const item = raw && typeof raw === 'object' ? raw as Record<string, unknown> : {};
  return {
    item: Number(item.item) | 0,
    slotType: Number(item.slotType) | 0,
    tradeable: Boolean(item.tradeable),
    included: Boolean(item.included),
    enchantment: typeof item.enchantment === 'string' ? item.enchantment : '',
  };
}

function normalizeTradeItems(raw: unknown): TradeItem[] {
  return Array.isArray(raw) ? raw.map(normalizeTradeItem) : [];
}

function activeSession(deps: BridgeDeps): TradeSession | undefined {
  const c = deps.clientRef.current;
  return c ? getSession(c) : undefined;
}

function sendChangeTrade(deps: BridgeDeps, c: ClientConnection, offer: boolean[]): boolean {
  try {
    const session = getSession(c);
    const count = normalizeSlotCount(session.ourSlotCount, offer.length || session.ourItems.length || 12);
    const normalizedOffer = toBoolArray(offer, count);
    const pkt = deps.proxy.packetFactory.createByName('CHANGETRADE');
    pkt.data.offer = normalizedOffer;
    pkt.modified = true;
    c.sendToServer(pkt);
    session.active = true;
    session.ourOffer = normalizedOffer.slice();
    return true;
  } catch (err) {
    Logger.warn('ScriptTrade', `change offer failed: ${(err as Error).message}`);
    return false;
  }
}

function onTradePacket(client: ClientConnection, packet: Packet): void {
  const name = String(packet.name ?? '').toUpperCase();
  const data = packet.data && typeof packet.data === 'object'
    ? packet.data as Record<string, unknown>
    : {};
  const session = getSession(client);

  if (name === 'TRADESTART') {
    const clientItems = normalizeTradeItems(data.clientItems);
    const partnerItems = normalizeTradeItems(data.partnerItems);
    session.active = true;
    session.ourSlotCount = normalizeSlotCount(clientItems.length, session.ourSlotCount);
    session.partnerSlotCount = normalizeSlotCount(partnerItems.length, session.partnerSlotCount);
    session.ourItems = clientItems;
    session.partnerItems = partnerItems;
    session.ourOffer = toBoolArray(extractTradeItemIncluded(clientItems), session.ourSlotCount);
    session.partnerOffer = toBoolArray(extractTradeItemIncluded(partnerItems), session.partnerSlotCount);
    session.partnerOfferFromTradeChanged = session.partnerOffer.slice();
    session.partnerName = typeof data.partnerName === 'string' ? data.partnerName : '';
    return;
  }

  if (name === 'TRADECHANGED') {
    session.active = true;
    const next = toBoolArray(data.offer, session.partnerSlotCount);
    session.partnerOffer = next;
    session.partnerOfferFromTradeChanged = next.slice();
    return;
  }

  if (name === 'CHANGETRADE') {
    session.active = true;
    session.ourOffer = toBoolArray(data.offer, session.ourSlotCount);
    return;
  }

  if (name === 'TRADEACCEPTED') {
    session.active = true;
    session.ourOffer = toBoolArray(data.clientOffer, session.ourSlotCount);
    session.partnerOffer = toBoolArray(data.partnerOffer, session.partnerSlotCount);
    // Keep partnerOfferFromTradeChanged — ACCEPTTRADE partner arrays must match last TRADECHANGED, not this snapshot.
    return;
  }

  if (name === 'TRADEDONE' || name === 'CANCELTRADE') {
    resetSession(client);
  }
}

function ensureHooks(deps: BridgeDeps): void {
  if (hooksInstalled) return;
  hooksInstalled = true;
  for (const packetName of ['TRADESTART', 'TRADECHANGED', 'CHANGETRADE', 'TRADEACCEPTED', 'TRADEDONE', 'CANCELTRADE']) {
    deps.proxy.hookPacket(packetName, (client, packet) => {
      try {
        onTradePacket(client, packet);
      } catch (err) {
        Logger.warn('ScriptTrade', `${packetName} hook failed: ${(err as Error).message}`);
      }
    });
  }
}

function currentClient(deps: BridgeDeps): ClientConnection | undefined {
  const c = deps.clientRef.current;
  if (!c?.connected) {
    Logger.warn('ScriptTrade', 'No active game client connection.');
    return undefined;
  }
  return c;
}

export function install(deps: BridgeDeps): void {
  ensureHooks(deps);

  trade.start = (playerName: string): boolean => {
    const c = currentClient(deps);
    if (!c) return false;

    const targetName = String(playerName ?? '').trim();
    if (!targetName) {
      Logger.warn('ScriptTrade', 'start: player name is required');
      return false;
    }

    try {
      const pkt = deps.proxy.packetFactory.createByName('REQUESTTRADE');
      pkt.data.name = targetName;
      pkt.modified = true;
      c.sendToServer(pkt);
      return true;
    } catch (err) {
      Logger.warn('ScriptTrade', `start failed: ${(err as Error).message}`);
      return false;
    }
  };

  trade.startTrade = (playerName: string): boolean => trade.start(playerName);

  trade.isActive = (): boolean => activeSession(deps)?.active ?? false;

  trade.getPartnerName = (): string => activeSession(deps)?.partnerName ?? '';

  trade.getOurItems = (): TradeItem[] => activeSession(deps)?.ourItems.map(cloneTradeItem) ?? [];

  trade.getPartnerItems = (): TradeItem[] => activeSession(deps)?.partnerItems.map(cloneTradeItem) ?? [];

  trade.getOurOffer = (): boolean[] => activeSession(deps)?.ourOffer.slice() ?? [];

  trade.getPartnerOffer = (): boolean[] => activeSession(deps)?.partnerOffer.slice() ?? [];

  trade.offer = (slotIndexes: number | number[]): boolean => {
    const c = currentClient(deps);
    if (!c) return false;

    const session = getSession(c);
    if (!session.active) {
      Logger.warn('ScriptTrade', 'offer: no active trade session');
      return false;
    }

    const indexes = Array.isArray(slotIndexes) ? slotIndexes : [slotIndexes];
    const offer = new Array<boolean>(normalizeSlotCount(session.ourSlotCount, session.ourItems.length || 12)).fill(false);
    for (const rawIndex of indexes) {
      const index = Math.trunc(Number(rawIndex));
      if (!Number.isFinite(index) || index < 0 || index >= offer.length) {
        Logger.warn('ScriptTrade', `offer: slot index ${String(rawIndex)} is out of range`);
        return false;
      }
      const item = session.ourItems[index];
      if (item && !item.tradeable) {
        Logger.warn('ScriptTrade', `offer: slot ${index} is not tradeable`);
        return false;
      }
      offer[index] = true;
    }

    return sendChangeTrade(deps, c, offer);
  };

  trade.offerAll = (): boolean => {
    const c = currentClient(deps);
    if (!c) return false;

    const session = getSession(c);
    if (!session.active) {
      Logger.warn('ScriptTrade', 'offerAll: no active trade session');
      return false;
    }

    const count = normalizeSlotCount(session.ourSlotCount, session.ourItems.length || 12);
    const offer = new Array<boolean>(count).fill(false);
    for (let i = 0; i < Math.min(session.ourItems.length, count); i++) {
      offer[i] = session.ourItems[i].tradeable;
    }
    return sendChangeTrade(deps, c, offer);
  };

  trade.clearOffer = (): boolean => {
    const c = currentClient(deps);
    if (!c) return false;

    const session = getSession(c);
    if (!session.active) {
      Logger.warn('ScriptTrade', 'clearOffer: no active trade session');
      return false;
    }

    const count = normalizeSlotCount(session.ourSlotCount, session.ourItems.length || 12);
    return sendChangeTrade(deps, c, new Array<boolean>(count).fill(false));
  };

  trade.accept = (): boolean => {
    const c = currentClient(deps);
    if (!c) return false;

    const session = getSession(c);
    if (!session.active) {
      Logger.warn('ScriptTrade', 'accept: no active trade session');
      return false;
    }

    try {
      const pkt = deps.proxy.packetFactory.createByName('ACCEPTTRADE');
      const ourCount = normalizeSlotCount(session.ourSlotCount, 12);
      const partnerCount = normalizeSlotCount(session.partnerSlotCount, 12);
      pkt.data.clientOffer = toBoolArray(session.ourOffer, ourCount);
      const partnerLine =
        session.partnerOfferFromTradeChanged.length > 0
          ? session.partnerOfferFromTradeChanged
          : session.partnerOffer;
      pkt.data.partnerOffer = toBoolArray(partnerLine, partnerCount);
      pkt.modified = true;
      c.sendToServer(pkt);
      return true;
    } catch (err) {
      Logger.warn('ScriptTrade', `accept failed: ${(err as Error).message}`);
      return false;
    }
  };

  trade.acceptTrade = (): boolean => trade.accept();

  trade.cancel = (): boolean => {
    const c = currentClient(deps);
    if (!c) return false;

    try {
      const pkt = deps.proxy.packetFactory.createByName('CANCELTRADE');
      pkt.modified = true;
      c.sendToServer(pkt);
      resetSession(c);
      return true;
    } catch (err) {
      Logger.warn('ScriptTrade', `cancel failed: ${(err as Error).message}`);
      return false;
    }
  };

  trade.cancelTrade = (): boolean => trade.cancel();
}
