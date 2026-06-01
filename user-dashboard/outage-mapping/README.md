# Outage Mapping Helpers

This folder contains standalone JavaScript helpers for dashboard, AMS node, or server-node outage logic.

It is not wired into `index.html`. The caller is responsible for rendering the UI, but `operator-health.js` can perform the AnyLog query-node health check.

## Files

- `thresholds.js`: outage threshold decisions, confidence score, and health/affected helpers.
- `geo.js`: raw distance and geometry helpers used by `thresholds.js`.
- `operator-health.js`: query-node health fetch using `test network with operator`, plus parser and node normalizer.

Most dashboard code should import from `operator-health.js` and `thresholds.js`. Import `geo.js` only when you need raw distance values for display, like showing the estimated outage span from one node to another.

## Node Shape

Each caller should normalize its health data into objects like this:

```js
const node = {
  id: "pi-1",
  name: "Pi-1",
  lat: 36.9993,
  lng: -122.0622,
  status: "down", // "normal" | "anomaly" | "down"
  online: false,
  anomalyStart: Date.now(),
  downSince: Date.now()
};
```

The helpers treat a node as affected if `status` is `"down"` or `"anomaly"`, or if `online`/`up` is `false`.

The helpers treat a node as healthy if `status` is `"normal"`, or if `online`/`up` is `true`.

## Displaying Node Health

Health can come from the AnyLog query-node command:

```text
test network with operator
```

In the dashboard, call it through nginx `/anylog` so CORS stays handled:

```js
import {
  fetchOperatorHealth,
  normalizeNodesWithOperatorHealth
} from "./outage-mapping/operator-health.js";

const health = await fetchOperatorHealth({ endpoint: "/anylog" });
const nodes = normalizeNodesWithOperatorHealth(configuredNodes, health.operators, {
  previousNodes
});
```

`fetchOperatorHealth(...)` returns:

```js
{
  queryNode: "/anylog",
  command: "test network with operator",
  raw: "...raw AnyLog table...",
  operators: [
    { name: "operator1", address: "127.0.0.1:32148", up: true, status: "up" },
    { name: "operator2", address: "127.0.0.1:32248", up: false, status: "down" }
  ]
}
```

`normalizeNodesWithOperatorHealth(...)` converts configured dashboard nodes into the node shape expected by `thresholds.js`. It maps node 1 to `operator1`, node 2 to `operator2`, and so on unless a node already has `operatorName`, `operatorAddress`, or `address`.

Example configured nodes:

```js
const configuredNodes = [
  { id: "pi-1", name: "Pi-1", lat: 36.9993, lng: -122.0622, operatorName: "operator1" },
  { id: "pi-2", name: "Pi-2", lat: 37.0015, lng: -122.0584, operatorName: "operator2" }
];
```

Then the dashboard can render health directly:

```js
function healthColor(node) {
  if (node.online === true) return "green";
  if (node.online === false) return "red";
  return "gray";
}
```

The `previousNodes` option is important because it preserves `downSince` while a node remains down. That gives the outage threshold formula a stable outage start time instead of resetting the time on every refresh.

## Deciding If Two Nodes Share An Outage

```js
import {
  shouldShareOutage,
  thresholdSignals,
  confidenceScore,
  confidenceLabel
} from "./outage-mapping/thresholds.js";
import {
  fetchOperatorHealth,
  normalizeNodesWithOperatorHealth
} from "./outage-mapping/operator-health.js";

const health = await fetchOperatorHealth({ endpoint: "/anylog" });
const nodes = normalizeNodesWithOperatorHealth(configuredNodes, health.operators, {
  previousNodes
});

const share = shouldShareOutage(nodeA, nodeB, nodes);
const signals = thresholdSignals(nodeA, nodeB, nodes);
const confidence = confidenceScore(nodeA, nodeB, nodes);

console.log({
  share,
  confidence,
  label: confidenceLabel(confidence),
  distanceMeters: signals.distanceMeters,
  timeGapMs: signals.timeGapMs
});
```

Default thresholds:

```js
{
  distanceThresholdMeters: 1000,
  timeThresholdMs: 5 * 60 * 1000,
  healthyCorridorMeters: 450
}
```

Two nodes share an outage when:

- both are affected
- they are within the distance threshold
- their outage/anomaly times are within the time threshold
- no healthy node sits between them within the healthy-node corridor

## Displaying Estimated Outage Size

Use `geo.js` when the UI needs a readable distance or a map extent.

```js
import {
  distanceMeters,
  formatDistance
} from "./outage-mapping/geo.js";

function outageSpanLabel(nodeA, nodeB) {
  const spanMeters = distanceMeters(nodeA, nodeB);
  return `${nodeA.name} to ${nodeB.name}: ${formatDistance(spanMeters)} estimated span`;
}
```

For a pair of linked down nodes, a simple dashboard label can be:

```js
if (shouldShareOutage(nodeA, nodeB, nodes)) {
  console.log(outageSpanLabel(nodeA, nodeB));
}
```

For a map, draw a line from `[nodeA.lat, nodeA.lng]` to `[nodeB.lat, nodeB.lng]`. The line length is the estimated span. If you want an outage area, draw a corridor or buffered region around that line. A simple first pass is:

```js
const spanMeters = distanceMeters(nodeA, nodeB);
const radiusMeters = Math.max(500, Math.min(1200, spanMeters / 2));
```

Use wording like:

```text
Estimated outage span: Pi-1 to Pi-2, about 420 m.
```

Avoid wording that claims confirmed utility connectivity unless you have real utility topology data.

## Grouping More Than Two Nodes

For many nodes, compare each affected pair with `shouldShareOutage(...)`, then group pairs that return `true`.

Minimal grouping approach:

```js
const affected = nodes.filter((node) => node.status !== "normal");
const links = [];

for (let i = 0; i < affected.length; i += 1) {
  for (let j = i + 1; j < affected.length; j += 1) {
    if (shouldShareOutage(affected[i], affected[j], nodes)) {
      links.push([affected[i], affected[j]]);
    }
  }
}
```

To fill the area inside the linked-node lines, collect the unique nodes from the links and render a filled polygon:

```js
import { polygonForNodes } from "./outage-mapping/geo.js";

const linkedNodes = [...new Map(
  links
    .flat()
    .map((node) => [node.id || node.name, node])
).values()];

const outagePolygon = polygonForNodes(linkedNodes);

if (outagePolygon.length >= 3) {
  L.polygon(outagePolygon, {
    color: "#f87171",
    weight: 1.5,
    opacity: 0.65,
    fillColor: "#f87171",
    fillOpacity: 0.22,
    interactive: false
  }).addTo(map);
}
```

The existing lines can stay if you still want them as boundaries. The polygon is the part that fills the inside area.

## Tuning

Pass threshold overrides when needed:

```js
const share = shouldShareOutage(nodeA, nodeB, nodes, {
  distanceThresholdMeters: 1500,
  timeThresholdMs: 10 * 60 * 1000,
  healthyCorridorMeters: 350
});
```

Use tighter thresholds for dense deployments and wider thresholds for sparse deployments.

## Important Limits

These helpers estimate outage relationships from node health, distance, timing, and healthy-node blockers. They do not:

- query voltage/anomaly rows from AnyLog
- know real electrical topology
- import substation data
- confirm utility connectivity
- render the dashboard UI

The intended flow is:

```text
AnyLog query-node health check -> normalized nodes -> threshold helpers -> dashboard/server rendering
```
