/**
 * Production build script for Realm Engine v1.
 *
 * Pipeline:
 *   1. Clean dist/
 *   2. Generate shared HMAC secret → BuildSecrets.h + __HANDSHAKE_KEY__
 *   3. Build C++ DLL via MSBuild (Release|x64)
 *   4. AES-256-GCM encrypt the DLL → assets/internal.bin
 *   5. Bundle core app with esbuild (embed DLL key + handshake key via define)
 *   6. Bundle each plugin individually (plugins/*.ts -> dist/plugins/*.js)
 *   7. Obfuscate all JS output with javascript-obfuscator
 *   8. Generate dist/integrity.json (SHA-256 hashes for anti-tamper checks)
 */

import * as esbuild from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync, rmSync, readdirSync, copyFileSync, existsSync, statSync } from 'fs';
import { join, resolve, basename } from 'path';
import { execSync } from 'child_process';
import { createHash, randomBytes, createCipheriv } from 'crypto';
import JavaScriptObfuscator from 'javascript-obfuscator';

const ADMIN_BUILD = process.argv.includes('--admin');

const ROOT = resolve(import.meta.dirname, '..');
const DIST = join(ROOT, 'dist');
const PLUGINS_SRC = join(ROOT, 'plugins');
const PLUGINS_DIST = join(DIST, 'plugins');
const DATA_DIR = join(ROOT, 'data');

// Locate the internal DLL repo as a sibling of this one. Supports both the
// canonical RealmEngineRotmg/internal clone name and the legacy DebugInternal
// layout. INTERNAL_DIR env var wins if set.
function findInternalDir() {
  const override = process.env.INTERNAL_DIR;
  if (override) return resolve(override);
  for (const name of ['internal', 'DebugInternal']) {
    const p = resolve(ROOT, '..', name);
    if (existsSync(join(p, 'il2cpp-dll-injection.sln'))) return p;
  }
  return resolve(ROOT, '..', 'internal');
}
const INTERNAL_DIR = findInternalDir();
const DLL_SLN = join(INTERNAL_DIR, 'il2cpp-dll-injection.sln');
const DLL_OUTPUT = join(INTERNAL_DIR, 'x64', 'Release', 'version.dll');
const DLL_DEST = join(ROOT, 'assets', 'internal.bin');
const BUILD_SECRETS_H = join(INTERNAL_DIR, 'src', 'ui', 'BuildSecrets.h');
const PLUGIN_SIG_PUBLIC_KEY = String(process.env.PLUGIN_BUNDLE_SIGNING_PUBLIC_KEY_PEM || '');
const PLUGIN_ENC_KEY = String(process.env.PLUGIN_BUNDLE_ENC_KEY || '');
const PACKET_DEFINITIONS_JSON = readFileSync(join(DATA_DIR, 'packet-definitions.json'), 'utf8');
const STAT_TYPES_JSON = readFileSync(join(DATA_DIR, 'stat-types.json'), 'utf8');
const SERVERS_JSON = readFileSync(join(DATA_DIR, 'servers.json'), 'utf8');

const EXCLUDED_PLUGINS = new Set([
  'auto-drink',    // directory plugin (plugins/auto-drink/), excluded from prod for now
]);
const ADMIN_ONLY_PLUGINS = new Set([
  'auto-ability.ts',
  'packet-logger.ts',
]);

// Core bundle config: no stringArray — it breaks bundled libraries like ws
// that use dynamic property access (this.options.WebSocket as constructor).
const OBFUSCATOR_CORE_CONFIG = {
  compact: true,
  controlFlowFlattening: true,
  controlFlowFlatteningThreshold: 0.4,
  deadCodeInjection: true,
  deadCodeInjectionThreshold: 0.15,
  debugProtection: false,
  identifierNamesGenerator: 'hexadecimal',
  renameGlobals: false,
  selfDefending: false,
  stringArray: false,
  transformObjectKeys: false,
  unicodeEscapeSequence: false,
};

// Plugin config: full obfuscation including stringArray + rc4 encoding.
// Plugins don't bundle ws or other libs with dynamic property patterns.
const OBFUSCATOR_PLUGIN_CONFIG = {
  compact: true,
  controlFlowFlattening: true,
  controlFlowFlatteningThreshold: 0.5,
  deadCodeInjection: true,
  deadCodeInjectionThreshold: 0.2,
  debugProtection: false,
  identifierNamesGenerator: 'hexadecimal',
  renameGlobals: false,
  selfDefending: false,
  stringArray: true,
  stringArrayEncoding: ['rc4'],
  stringArrayThreshold: 0.75,
  transformObjectKeys: false,
  unicodeEscapeSequence: false,
};

function log(msg) {
  console.log(`[build-prod] ${msg}`);
}

function fileSize(path) {
  try { return (statSync(path).size / 1024).toFixed(1) + ' KB'; }
  catch { return '?'; }
}

// ── Step 1: Clean ────────────────────────────────────────────────────────────

log(`Build mode: ${ADMIN_BUILD ? 'ADMIN (dev features included)' : 'USER (admin features stripped)'}`);
if (!PLUGIN_SIG_PUBLIC_KEY.trim()) {
  log('Warning: PLUGIN_BUNDLE_SIGNING_PUBLIC_KEY_PEM not set; production remote plugin loading will be blocked.');
}
if (!/^[0-9a-fA-F]{64}$/.test(PLUGIN_ENC_KEY)) {
  log('Warning: PLUGIN_BUNDLE_ENC_KEY missing/invalid; production remote plugin loading will be blocked.');
}
log('Cleaning dist/...');
rmSync(DIST, { recursive: true, force: true });
mkdirSync(PLUGINS_DIST, { recursive: true });

// ── Step 2: Generate shared handshake secret ────────────────────────────────

log('Generating shared handshake secret + pipe name...');
const handshakeKey = randomBytes(32).toString('hex');
const pipeSuffix = randomBytes(12).toString('hex');
const pipeName = `\\\\.\\pipe\\lfg-${pipeSuffix}`;

// Write BuildSecrets.h for the C++ DLL (compiled into the binary via xorstr).
// Note: C++ string literal escaping doubles the backslashes again, so the macro
// stores the same \\.\pipe\... string that CreateNamedPipeA expects at runtime.
const secretsHeader = `#pragma once
// AUTO-GENERATED by build-prod.mjs — do not edit or commit.
// Shared HMAC key + pipe name for bot-client ↔ DLL mutual authentication.
#define BUILD_HANDSHAKE_KEY "${handshakeKey}"
#define BUILD_PIPE_NAME "${pipeName.replace(/\\/g, '\\\\')}"
`;
writeFileSync(BUILD_SECRETS_H, secretsHeader);
log('BuildSecrets.h written');

// ── Step 3: Build C++ DLL ────────────────────────────────────────────────────

log('Building C++ DLL (Release|x64)...');
if (!existsSync(DLL_SLN)) {
  console.error(`[build-prod] ERROR: Solution not found: ${DLL_SLN}`);
  process.exit(1);
}

// Locate MSBuild: prefer PATH, then vswhere, then common hard-coded paths.
function findMSBuild() {
  // 1. Already on PATH (Developer Command Prompt)?
  try { execSync('msbuild /version /nologo', { stdio: 'pipe' }); return 'msbuild'; } catch {}

  // 2. vswhere (ships with VS 2017+ installer, always at this fixed location).
  // -prerelease lets it discover VS2026 previews; we drop -requires because
  // some VS2026 installs report different component IDs.
  const vswhere = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe';
  if (existsSync(vswhere)) {
    for (const args of [
      '-latest -prerelease -find MSBuild\\**\\Bin\\MSBuild.exe',
      '-latest -prerelease -requires Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe',
      '-latest -requires Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe',
    ]) {
      try {
        const found = execSync(`"${vswhere}" ${args}`, { stdio: 'pipe' })
          .toString().trim().split('\n')[0].trim();
        if (found && existsSync(found)) return `"${found}"`;
      } catch {}
    }
  }

  // 3. Hard-coded fallbacks. VS2026 internal version is 18 — the installer puts
  // BuildTools at \18\BuildTools rather than \2026\BuildTools.
  const candidates = [
    // VS2026 (internal version 18)
    'C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\MSBuild\\Current\\Bin\\MSBuild.exe',
    'C:\\Program Files\\Microsoft Visual Studio\\18\\Professional\\MSBuild\\Current\\Bin\\MSBuild.exe',
    'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe',
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools\\MSBuild\\Current\\Bin\\MSBuild.exe',
    // VS2022
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\MSBuild\\Current\\Bin\\MSBuild.exe',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\MSBuild\\Current\\Bin\\MSBuild.exe',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe',
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\MSBuild\\Current\\Bin\\MSBuild.exe',
  ];
  for (const c of candidates) if (existsSync(c)) return `"${c}"`;

  return null;
}

const msbuild = findMSBuild();
if (!msbuild) {
  console.error('[build-prod] ERROR: MSBuild not found. Install Visual Studio 2022 or 2026 (any edition) or Build Tools.');
  console.error('[build-prod] Alternatively, run this script from a Developer Command Prompt.');
  process.exit(1);
}
log(`Using MSBuild: ${msbuild}`);

try {
  // /t:Rebuild (not incremental): BuildSecrets.h is regenerated above with a
  // fresh per-build handshake key. An incremental build can leave Handshake.obj
  // "up to date" and ship the DLL with a stale key while the JS bundle gets the
  // new one — the DLL then injects but fails pipe auth, so every setFeature
  // (dodge, hitbox, ...) is silently dropped. A clean rebuild guarantees the
  // key in the DLL matches __HANDSHAKE_KEY__ baked into the bundle.
  // 10 min: a clean full compile of the DLL (~7000+ functions) can exceed
  // the old 2-minute cap, especially on CI or after node-gyp wipes caches.
  execSync(
    `${msbuild} "${DLL_SLN}" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal`,
    { stdio: 'inherit', timeout: 600000 }
  );
} catch (err) {
  console.error('[build-prod] ERROR: MSBuild failed.');
  process.exit(1);
}

if (!existsSync(DLL_OUTPUT)) {
  console.error(`[build-prod] ERROR: DLL not found after build: ${DLL_OUTPUT}`);
  process.exit(1);
}

log(`DLL built: ${fileSize(DLL_OUTPUT)}`);

// ── Step 3: AES-encrypt DLL ──────────────────────────────────────────────────

log('Encrypting DLL (AES-256-GCM)...');

const dllKey = randomBytes(32);
const dllIv  = randomBytes(16);
const cipher = createCipheriv('aes-256-gcm', dllKey, dllIv);
const dllPlain = readFileSync(DLL_OUTPUT);
const encryptedDll = Buffer.concat([cipher.update(dllPlain), cipher.final()]);
const authTag = cipher.getAuthTag();

// Pack as: [16 IV][16 authTag][...ciphertext]
const packed = Buffer.concat([dllIv, authTag, encryptedDll]);
writeFileSync(DLL_DEST, packed);

// Hex key will be baked into the JS bundle via esbuild `define` below.
const dllKeyHex = dllKey.toString('hex');

log(`DLL encrypted: ${fileSize(DLL_OUTPUT)} → ${fileSize(DLL_DEST)}`);

// ── Step 4: Bundle core ──────────────────────────────────────────────────────

log('Bundling core application...');
await esbuild.build({
  entryPoints: [join(ROOT, 'src', 'index.ts')],
  bundle: true,
  platform: 'node',
  target: 'node20',
  format: 'cjs',
  outfile: join(DIST, 'app.cjs'),
  minify: true,
  sourcemap: false,
  treeShaking: true,
  // 'electron' MUST be external — bundling it pulls in node_modules/electron/
  // index.js (the launcher stub for *spawning* electron from outside) and
  // throws "Electron failed to install correctly" at runtime inside the
  // packaged app. Marked external so `import { shell } from 'electron'`
  // resolves to electron's built-in module loader at runtime.
  external: ['koffi', 'sharp', 'electron'],
  // esbuild CJS output replaces import.meta with {}, making import.meta.url = undefined.
  // Inject a shim at the top of the bundle and point define at it so
  // dirname(fileURLToPath(import.meta.url)) resolves to the bundle's own directory.
  banner: {
    js: 'var __importMetaUrl=require("url").pathToFileURL(__filename).href;',
  },
  define: {
    PRODUCTION: '"true"',
    __DLL_KEY__: JSON.stringify(dllKeyHex),
    __HANDSHAKE_KEY__: JSON.stringify(handshakeKey),
    __PIPE_NAME__: JSON.stringify(pipeName),
    __ADMIN_BUILD__: String(ADMIN_BUILD),
    __PLUGIN_BUNDLE_SIGNING_PUBLIC_KEY__: JSON.stringify(PLUGIN_SIG_PUBLIC_KEY),
    __PLUGIN_BUNDLE_ENC_KEY__: JSON.stringify(PLUGIN_ENC_KEY),
    __PACKET_DEFINITIONS_JSON__: JSON.stringify(PACKET_DEFINITIONS_JSON),
    __STAT_TYPES_JSON__: JSON.stringify(STAT_TYPES_JSON),
    __SERVERS_JSON__: JSON.stringify(SERVERS_JSON),
    'import.meta.url': '__importMetaUrl',
  },
  logLevel: 'warning',
});
log(`Core bundled: ${fileSize(join(DIST, 'app.cjs'))}`);

// ── Step 5: Bundle plugins ───────────────────────────────────────────────────

log('Bundling plugins...');
const excludedPluginFiles = new Set(EXCLUDED_PLUGINS);
if (!ADMIN_BUILD) {
  for (const file of ADMIN_ONLY_PLUGINS)
    excludedPluginFiles.add(file);
}
// Discover plugins: top-level `*.ts` files, plus directory plugins
// (`<name>/index.ts`, bundled to `<name>.js`). Exclusion is keyed by the file
// name for files and by the folder name for directory plugins.
const pluginEntries = [];
for (const name of readdirSync(PLUGINS_SRC)) {
  const full = join(PLUGINS_SRC, name);
  const st = statSync(full);
  if (st.isFile()) {
    if (name.endsWith('.ts') && !excludedPluginFiles.has(name)) {
      pluginEntries.push({ entry: full, outName: name.replace(/\.ts$/, '.js') });
    }
  } else if (st.isDirectory() && !excludedPluginFiles.has(name)) {
    const indexEntry = join(full, 'index.ts');
    if (existsSync(indexEntry)) {
      pluginEntries.push({ entry: indexEntry, outName: `${name}.js` });
    }
  }
}

for (const { entry, outName } of pluginEntries) {
  await esbuild.build({
    entryPoints: [entry],
    bundle: true,
    platform: 'node',
    target: 'node20',
    format: 'esm',
    outfile: join(PLUGINS_DIST, outName),
    minify: true,
    sourcemap: false,
    treeShaking: true,
    // 'electron' MUST be external — bundling it pulls in node_modules/electron/
  // index.js (the launcher stub for *spawning* electron from outside) and
  // throws "Electron failed to install correctly" at runtime inside the
  // packaged app. Marked external so `import { shell } from 'electron'`
  // resolves to electron's built-in module loader at runtime.
  external: ['koffi', 'sharp', 'electron'],
    define: {
      __ADMIN_BUILD__: String(ADMIN_BUILD),
      __PACKET_DEFINITIONS_JSON__: JSON.stringify(PACKET_DEFINITIONS_JSON),
      __STAT_TYPES_JSON__: JSON.stringify(STAT_TYPES_JSON),
      __SERVERS_JSON__: JSON.stringify(SERVERS_JSON),
    },
    logLevel: 'warning',
  });
}
log(`${pluginEntries.length} plugins bundled`);

// ── Step 6: Obfuscate ────────────────────────────────────────────────────────

log('Obfuscating core (no stringArray — ws compat)...');
const appCode = readFileSync(join(DIST, 'app.cjs'), 'utf-8');
const obfuscatedApp = JavaScriptObfuscator.obfuscate(appCode, OBFUSCATOR_CORE_CONFIG).getObfuscatedCode();
writeFileSync(join(DIST, 'app.cjs'), obfuscatedApp);
log(`Core obfuscated: ${fileSize(join(DIST, 'app.cjs'))}`);

log('Obfuscating plugins (full + stringArray rc4)...');
for (const file of readdirSync(PLUGINS_DIST).filter(f => f.endsWith('.js'))) {
  const filePath = join(PLUGINS_DIST, file);
  const code = readFileSync(filePath, 'utf-8');
  const obfuscated = JavaScriptObfuscator.obfuscate(code, OBFUSCATOR_PLUGIN_CONFIG).getObfuscatedCode();
  writeFileSync(filePath, obfuscated);
}
log('Plugins obfuscated');

// ── Step 6b: Strip admin features from dashboard (user builds only) ──────────

// Copy src/dev/public → dist/public (staging copy — never modify source)
const PUBLIC_SRC = join(ROOT, 'src', 'dev', 'public');
const PUBLIC_DIST = join(DIST, 'public');
mkdirSync(PUBLIC_DIST, { recursive: true });
for (const f of readdirSync(PUBLIC_SRC)) {
  const src = join(PUBLIC_SRC, f);
  if (statSync(src).isFile()) copyFileSync(src, join(PUBLIC_DIST, f));
}
// Copy subdirectories (e.g. enchantments/)
for (const f of readdirSync(PUBLIC_SRC)) {
  const src = join(PUBLIC_SRC, f);
  if (statSync(src).isDirectory()) {
    const destDir = join(PUBLIC_DIST, f);
    mkdirSync(destDir, { recursive: true });
    for (const sub of readdirSync(src)) {
      const subSrc = join(src, sub);
      if (statSync(subSrc).isFile()) copyFileSync(subSrc, join(destDir, sub));
    }
  }
}

// Inject __ADMIN_BUILD__ flag into app.js (CSS hides admin-only elements;
// JS uses the flag to lock out admin mode entirely in user builds).
// HTML is NOT stripped — removing elements breaks JS event listeners.
{
  const appJsPath = join(PUBLIC_DIST, 'app.js');
  if (existsSync(appJsPath)) {
    let js = readFileSync(appJsPath, 'utf-8');
    js = `var __ADMIN_BUILD__=${ADMIN_BUILD};\n` + js;
    writeFileSync(appJsPath, js);
  }
  log(ADMIN_BUILD ? 'Admin build — all features enabled' : 'User build — admin features locked');
}

// ── Step 7: Generate integrity manifest ──────────────────────────────────────

log('Generating integrity manifest...');

/**
 * Compute SHA-256 hex digest for a file.
 * @param {string} filePath
 */
function hashFile(filePath) {
  const data = readFileSync(filePath);
  return createHash('sha256').update(data).digest('hex');
}

/**
 * Files to include in the manifest.
 *
 * - electron/main.cjs and electron/preload.cjs are not obfuscated and
 *   are verified by dist/app.cjs at runtime.
 * - dist/app.cjs is the obfuscated bundle; verified by electron/main.cjs
 *   before it is forked.
 * - dist/plugins/*.js are verified alongside the core bundle.
 *
 * Paths are relative to ROOT, using forward slashes.
 */
const MANIFEST_FILES = [
  'electron/main.cjs',
  'electron/preload.cjs',
  'electron/security.cjs',
  'electron/loading.html',
  'dist/app.cjs',
  // Plugins are added dynamically below.
];

for (const file of readdirSync(PLUGINS_DIST).filter(f => f.endsWith('.js'))) {
  MANIFEST_FILES.push(`dist/plugins/${file}`);
}

const manifest = MANIFEST_FILES.map(relPath => {
  const fullPath = resolve(ROOT, relPath.replace(/\//g, process.platform === 'win32' ? '\\' : '/'));
  if (!existsSync(fullPath)) {
    console.error(`[build-prod] ERROR: manifest target not found: ${fullPath}`);
    process.exit(1);
  }
  return { path: relPath, sha256: hashFile(fullPath) };
});

writeFileSync(join(DIST, 'integrity.json'), JSON.stringify(manifest, null, 2));
log(`Integrity manifest written: dist/integrity.json (${manifest.length} entries)`);

// Best-effort cleanup: keep the generated handshake secret header out of disk
// between builds. It is regenerated for each production build anyway.
try {
  rmSync(BUILD_SECRETS_H, { force: true });
  log('BuildSecrets.h removed from source tree (post-build cleanup)');
} catch {
  log('Warning: failed to remove BuildSecrets.h after build');
}

// ── Done ─────────────────────────────────────────────────────────────────────

log('');
log('Production build complete!');
log('');
log('Output:');
log(`  Core:      dist/app.cjs (${fileSize(join(DIST, 'app.cjs'))})`);
log(`  Plugins:   dist/plugins/ (${pluginFiles.length} files)`);
log(`  DLL:       assets/internal.bin (${fileSize(DLL_DEST)})`);
log(`  Integrity: dist/integrity.json (${manifest.length} entries)`);
log('');
log('Run "npm run dist" to package with electron-builder.');
