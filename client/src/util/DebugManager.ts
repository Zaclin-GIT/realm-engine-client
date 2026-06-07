/**
 *  ╔════════════════════════════════════════════════════════════════════════════╗
 *  ║                      ⚙   D E B U G   M A N A G E R   ⚙                    ║
 *  ╠════════════════════════════════════════════════════════════════════════════╣
 *  ║  Central switchboard for Realm Engine's verbose / diagnostic logging.      ║
 *  ║                                                                            ║
 *  ║  Hundreds of noisy lines used to spew on every startup — per-plugin        ║
 *  ║  setting dumps, HELLO/RECONNECT byte traces, [DIAG-*] socket traces,       ║
 *  ║  script-bridge probes. They are now routed through Logger.debug(channel),  ║
 *  ║  and are OFF by default. Flip them back on per channel with RE_DEBUG:      ║
 *  ║                                                                            ║
 *  ║      RE_DEBUG=all                  → every channel                         ║
 *  ║      RE_DEBUG=reconnect,proxy      → just those two                        ║
 *  ║      RE_DEBUG=plugin-config        → the per-setting startup dump          ║
 *  ║                                                                            ║
 *  ║  (PowerShell:  $env:RE_DEBUG="all"; npm run electron )                     ║
 *  ║                                                                            ║
 *  ║  >>> Adding new verbose logging? Pick (or add) a channel in DEBUG_CHANNELS ║
 *  ║  >>> below and call Logger.debug(channel, module, msg) — never Logger.log  ║
 *  ║  >>> for spam. That is the whole point of this file.                       ║
 *  ╚════════════════════════════════════════════════════════════════════════════╝
 */

/** Known verbose channels. Add new ones here (and document them below). */
export type DebugChannel =
  | 'plugin-config'
  | 'plugin-load'
  | 'proxy'
  | 'reconnect'
  | 'scripts'
  | 'accounts';

/** Every channel + a one-line description (also shown in the startup hint). */
export const DEBUG_CHANNELS: Record<DebugChannel, string> = {
  'plugin-config': 'Per-plugin setting values, enabled/disabled state + hotkeys at load',
  'plugin-load':   'One line per bundled/user/remote plugin as it loads',
  'proxy':         '[DIAG-*] client/server socket lifecycle traces',
  'reconnect':     'HELLO / RECONNECT key + byte-level diagnostics',
  'scripts':       'Script SDK UI-bridge / ScriptHost probe traces',
  'accounts':      'DevServer account-file reads — WARNING: dumps raw creds incl. password',
};

/**
 * Reads the RE_DEBUG env var once at process start and answers `enabled(channel)`.
 * Prints a single conspicuous hint the first time anything is gated, so the next
 * dev sees how to turn the firehose back on instead of wondering where it went.
 */
export class DebugManager {
  private static readonly active: Set<string> = DebugManager.parseEnv();
  private static hintShown = false;

  private static parseEnv(): Set<string> {
    const raw = (process.env.RE_DEBUG ?? '').trim().toLowerCase();
    if (!raw) return new Set();
    if (raw === 'all' || raw === '*' || raw === '1' || raw === 'true') {
      return new Set(Object.keys(DEBUG_CHANNELS));
    }
    return new Set(raw.split(/[\s,]+/).filter(Boolean));
  }

  /** True when `channel` should emit. Cheap after the first call. */
  static enabled(channel: DebugChannel): boolean {
    DebugManager.maybeShowHint();
    return DebugManager.active.has(channel);
  }

  /** True if any channel at all is on (gate whole expensive blocks with this). */
  static get anyEnabled(): boolean {
    return DebugManager.active.size > 0;
  }

  private static maybeShowHint(): void {
    if (DebugManager.hintShown) return;
    DebugManager.hintShown = true;
    const ts = new Date().toISOString().slice(11, 23);
    const all = Object.keys(DEBUG_CHANNELS).join(', ');
    if (DebugManager.active.size === 0) {
      console.log(
        `[${ts}] [Debug] Verbose logging OFF — channels: ${all}. ` +
        `Enable with RE_DEBUG=all or RE_DEBUG=<channel,…> (see util/DebugManager.ts).`,
      );
    } else {
      const on = [...DebugManager.active].join(', ');
      console.log(
        `[${ts}] [Debug] Verbose channels ON: ${on}. ` +
        `(RE_DEBUG=all for every channel — see util/DebugManager.ts).`,
      );
    }
  }
}
