import {readFile} from 'node:fs/promises';

const wasmPath = process.argv[2];
if (!wasmPath) throw new Error('Usage: node run.mjs path/to/frames_test.wasm');
const {instance} = await WebAssembly.instantiate(await readFile(wasmPath), {});
const cases = [
  'test_startup_rollback',
  'test_copy_latest_front',
  'test_disable_validation_contention',
  'test_max_frame',
  'test_metrics_and_repeated_handover',
];
for (const name of cases) {
  const line = instance.exports[name]();
  if (line) throw new Error(`${name} failed at frames_test.c:${line}`);
  console.log(`PASS ${name}`);
}
console.log('5 production app_frames.c regression groups passed.');
