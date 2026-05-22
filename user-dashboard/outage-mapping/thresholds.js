import {
  distanceMeters,
  distanceToSegmentMeters
} from "./geo.js";

export const DEFAULT_THRESHOLDS = {
  distanceThresholdMeters: 1000,
  timeThresholdMs: 5 * 60 * 1000,
  healthyCorridorMeters: 450
};

export function isAffectedNode(node) {
  return node.status === "down"
    || node.status === "anomaly"
    || node.up === false
    || node.online === false;
}

export function isHealthyNode(node) {
  return node.status === "normal"
    || node.up === true
    || node.online === true;
}

export function nodeEventTime(node) {
  return node.anomalyStart
    || node.downSince
    || node.lastDownAt
    || null;
}

export function timeGapMs(first, second) {
  const firstTime = nodeEventTime(first);
  const secondTime = nodeEventTime(second);

  if (!firstTime || !secondTime) {
    return Infinity;
  }

  return Math.abs(firstTime - secondTime);
}

export function hasHealthyNodeBetween(first, second, nodes, options = {}) {
  const corridorMeters = options.healthyCorridorMeters
    || DEFAULT_THRESHOLDS.healthyCorridorMeters;
  const endpointDistance = distanceMeters(first, second);

  return nodes.filter(isHealthyNode).some((node) => {
    const nearSegment = distanceToSegmentMeters(node, first, second) <= corridorMeters;
    const betweenEndpoints = distanceMeters(node, first) < endpointDistance
      && distanceMeters(node, second) < endpointDistance;

    return nearSegment && betweenEndpoints;
  });
}

export function thresholdSignals(first, second, nodes = [], options = {}) {
  const distanceThresholdMeters = options.distanceThresholdMeters
    || DEFAULT_THRESHOLDS.distanceThresholdMeters;
  const timeThresholdMs = options.timeThresholdMs
    || DEFAULT_THRESHOLDS.timeThresholdMs;
  const distance = distanceMeters(first, second);
  const timeGap = timeGapMs(first, second);
  const nodesAreClose = distance <= distanceThresholdMeters;
  const anomalyTimesAreClose = timeGap <= timeThresholdMs;
  const healthyNodeBetween = hasHealthyNodeBetween(first, second, nodes, options);
  const sameNearestSubstation = Boolean(
    first.nearestSubstationId
      && first.nearestSubstationId === second.nearestSubstationId
  );
  const sameNearestPowerLineNetwork = Boolean(
    first.nearestPowerLineNetworkId
      && first.nearestPowerLineNetworkId === second.nearestPowerLineNetworkId
  );
  const healthyNodeOnSameMappedLine = nodes.filter(isHealthyNode).some((node) => (
    node.nearestPowerLineNetworkId
      && node.nearestPowerLineNetworkId === first.nearestPowerLineNetworkId
      && node.nearestPowerLineNetworkId === second.nearestPowerLineNetworkId
  ));

  return {
    distanceMeters: distance,
    timeGapMs: timeGap,
    nodesAreClose,
    anomalyTimesAreClose,
    healthyNodeBetween,
    sameNearestSubstation,
    sameNearestPowerLineNetwork,
    healthyNodeOnSameMappedLine
  };
}

export function confidenceScore(first, second, nodes = [], options = {}) {
  const signals = thresholdSignals(first, second, nodes, options);
  let confidence = 0.4;

  if (signals.nodesAreClose) confidence += 0.2;
  if (signals.anomalyTimesAreClose) confidence += 0.2;
  if (signals.sameNearestSubstation) confidence += 0.1;
  if (signals.sameNearestPowerLineNetwork) confidence += 0.1;
  if (signals.healthyNodeBetween) confidence -= 0.3;
  if (signals.healthyNodeOnSameMappedLine) confidence -= 0.3;

  return Math.max(0, Math.min(1, confidence));
}

export function confidenceLabel(confidence) {
  if (confidence < 0.3) {
    return "weak estimate";
  }

  if (confidence < 0.7) {
    return "possible shared outage";
  }

  return "likely shared outage";
}

export function shouldShareOutage(first, second, nodes = [], options = {}) {
  if (!isAffectedNode(first) || !isAffectedNode(second)) {
    return false;
  }

  const signals = thresholdSignals(first, second, nodes, options);

  return signals.nodesAreClose
    && signals.anomalyTimesAreClose
    && !signals.healthyNodeBetween;
}
