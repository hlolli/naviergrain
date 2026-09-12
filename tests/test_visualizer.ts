import assert from "node:assert/strict";
import {periodicSegments, validateView, viewSize} from "../web/visualizer";
assert.deepEqual(periodicSegments([.9, .3], [1.1, .3]).length, 2);
const corner = periodicSegments([.9, .9], [1.1, 1.1]);
assert.equal(corner.length, 2);
assert.deepEqual(corner[0]![1], [1, 1]);
assert.deepEqual(corner[1]![0], [0, 0]);
assert.equal(periodicSegments([.2, .5], [-1.2, .5]).length, 3);
assert.equal(periodicSegments([0, 0], [9, 1]).length, 0);
assert.equal(periodicSegments([NaN, 0], [1, 1]).length, 0);
assert.equal(periodicSegments([2 ** 53, 0], [2 ** 53 + 2, 0]).length, 2);
let segments = 0;
for (let i = 0; i < 1000; i++) {
  const a: [number, number] = [Math.sin(i) * 4, Math.cos(i) * 4];
  const b: [number, number] = [a[0] + Math.sin(i * 2) * 2, a[1] + Math.cos(i * 2) * 2];
  let dx = 0, dy = 0;
  for (const [start, end] of periodicSegments(a, b)) {
    for (const n of [...start, ...end]) assert(n >= -1e-12 && n <= 1 + 1e-12);
    dx += end[0] - start[0]; dy += end[1] - start[1]; segments++;
  }
  assert(Math.abs(dx - (b[0] - a[0])) < 1e-12);
  assert(Math.abs(dy - (b[1] - a[1])) < 1e-12);
}
const view = new Float64Array(viewSize);
view[0] = 1; view[1] = viewSize; view[7] = 16;
validateView(view);
view[8] = 129; assert.throws(() => validateView(view));
view[8] = 0; view[300] = NaN; assert.throws(() => validateView(view));
console.log(JSON.stringify({periodicCases: 1000, segments, cornerAndMultipleWraps: true}));
