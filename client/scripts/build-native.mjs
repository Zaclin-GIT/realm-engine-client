import { spawnSync } from 'child_process';
import { createRequire } from 'module';
import { existsSync, mkdirSync, writeFileSync } from 'fs';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const nativeDir = resolve(__dirname, '..', 'electron', 'native');
const nativeOutDir = resolve(__dirname, '..', 'src', 'native');
const helloEventStub = resolve(nativeOutDir, 'hello-event.js');
const rotmgSharedStub = resolve(nativeOutDir, 'rotmg-shared.js');

if (!existsSync(nativeDir)) {
  console.log('[build:native] electron/native not found — writing stubs and skipping native build.');
  mkdirSync(nativeOutDir, { recursive: true });
  if (!existsSync(helloEventStub)) {
    writeFileSync(helloEventStub, '// Stub — real implementation provided by the Windows native build.\nexport function signalHelloEvent() {}\n');
  }
  if (!existsSync(rotmgSharedStub)) {
    writeFileSync(rotmgSharedStub, '// Stub — real implementation provided by the Windows native build.\nexport const DEFENSE_UNSET = -1;\nexport function openShared() { return false; }\nexport function readPosition() { return null; }\n');
  }
  process.exit(0);
}

function run(command, args, opts = {}) {
  // Node 20.12+ blocks spawnSync of .cmd/.bat without shell:true (CVE-2024-27980,
  // "BatBadBut"). Default to shell:true on Windows so npm.cmd / npx.cmd work.
  const result = spawnSync(command, args, {
    stdio: 'inherit',
    shell: process.platform === 'win32',
    ...opts,
  });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function hasNodeAddonApi() {
  try {
    require.resolve('node-addon-api/package.json');
    return true;
  } catch {
    return false;
  }
}

if (!hasNodeAddonApi()) {
  console.log('[build:native] node-addon-api missing; installing...');
  // npm.cmd is a batch file on Windows and must go through the shell.
  // node (process.execPath) is a real binary — no shell needed, and
  // shell:true would split paths containing spaces like "C:\Program Files\..."
  run(
    process.platform === 'win32' ? 'npm.cmd' : 'npm',
    ['install', 'node-addon-api', '--no-save'],
    { shell: process.platform === 'win32' }
  );
}

const nodeGypBin = require.resolve('node-gyp/bin/node-gyp.js');
// process.execPath is a real .exe (e.g. C:\Program Files\nodejs\node.exe). Force
// shell:false so a path with spaces isn't mangled by cmd.exe.
run(process.execPath, [nodeGypBin, 'rebuild', '--directory', 'electron/native'], { shell: false });
