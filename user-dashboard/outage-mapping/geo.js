export function distanceMeters(first, second) {
  if (!Number.isFinite(first?.lat) || !Number.isFinite(first?.lng)
    || !Number.isFinite(second?.lat) || !Number.isFinite(second?.lng)) {
    return Infinity;
  }

  const earthRadiusMeters = 6371000;
  const lat1 = first.lat * Math.PI / 180;
  const lat2 = second.lat * Math.PI / 180;
  const deltaLat = (second.lat - first.lat) * Math.PI / 180;
  const deltaLng = (second.lng - first.lng) * Math.PI / 180;
  const a = Math.sin(deltaLat / 2) ** 2
    + Math.cos(lat1) * Math.cos(lat2) * Math.sin(deltaLng / 2) ** 2;

  return earthRadiusMeters * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

export function projectedPoint(point, origin) {
  const metersPerDegreeLat = 111320;
  const metersPerDegreeLng = 111320 * Math.cos(origin.lat * Math.PI / 180);

  return {
    x: (point.lng - origin.lng) * metersPerDegreeLng,
    y: (point.lat - origin.lat) * metersPerDegreeLat
  };
}

export function distanceToSegmentMeters(point, start, end) {
  const projectedStart = projectedPoint(start, start);
  const projectedEnd = projectedPoint(end, start);
  const projectedPointValue = projectedPoint(point, start);
  const deltaX = projectedEnd.x - projectedStart.x;
  const deltaY = projectedEnd.y - projectedStart.y;
  const lengthSquared = deltaX ** 2 + deltaY ** 2;

  if (lengthSquared === 0) {
    return distanceMeters(point, start);
  }

  const projection = Math.max(0, Math.min(1, (
    ((projectedPointValue.x - projectedStart.x) * deltaX)
    + ((projectedPointValue.y - projectedStart.y) * deltaY)
  ) / lengthSquared));

  const closest = {
    x: projectedStart.x + projection * deltaX,
    y: projectedStart.y + projection * deltaY
  };

  return Math.hypot(projectedPointValue.x - closest.x, projectedPointValue.y - closest.y);
}

export function closestPointOnSegment(point, start, end) {
  const projectedStart = projectedPoint(start, start);
  const projectedEnd = projectedPoint(end, start);
  const projectedPointValue = projectedPoint(point, start);
  const deltaX = projectedEnd.x - projectedStart.x;
  const deltaY = projectedEnd.y - projectedStart.y;
  const lengthSquared = deltaX ** 2 + deltaY ** 2;

  if (lengthSquared === 0) {
    return start;
  }

  const projection = Math.max(0, Math.min(1, (
    ((projectedPointValue.x - projectedStart.x) * deltaX)
    + ((projectedPointValue.y - projectedStart.y) * deltaY)
  ) / lengthSquared));

  return {
    lat: start.lat + (end.lat - start.lat) * projection,
    lng: start.lng + (end.lng - start.lng) * projection
  };
}

export function closestPointOnFeature(point, feature) {
  if (!feature || feature.geometry.length < 2) {
    return feature?.center || null;
  }

  return feature.geometry
    .slice(0, -1)
    .map((start, index) => closestPointOnSegment(point, start, feature.geometry[index + 1]))
    .map((candidate) => ({
      ...candidate,
      distanceMeters: distanceMeters(point, candidate)
    }))
    .sort((first, second) => first.distanceMeters - second.distanceMeters)[0];
}

export function distanceToFeatureMeters(point, feature) {
  if (!feature) {
    return Infinity;
  }

  if (feature.geometry.length < 2) {
    return distanceMeters(point, feature.center);
  }

  let shortest = Infinity;
  for (let index = 0; index < feature.geometry.length - 1; index += 1) {
    shortest = Math.min(
      shortest,
      distanceToSegmentMeters(point, feature.geometry[index], feature.geometry[index + 1])
    );
  }

  return shortest;
}

export function formatDistance(meters) {
  if (!Number.isFinite(meters)) {
    return "unknown distance";
  }

  if (meters < 1000) {
    return `${Math.round(meters)} m`;
  }

  return `${(meters / 1000).toFixed(1)} km`;
}

export function areaCenter(members) {
  return {
    lat: members.reduce((sum, member) => sum + member.lat, 0) / members.length,
    lng: members.reduce((sum, member) => sum + member.lng, 0) / members.length
  };
}

function cross(origin, first, second) {
  return (first.x - origin.x) * (second.y - origin.y)
    - (first.y - origin.y) * (second.x - origin.x);
}

export function polygonForNodes(nodes) {
  const usableNodes = nodes.filter((node) => (
    Number.isFinite(node?.lat) && Number.isFinite(node?.lng)
  ));

  if (usableNodes.length < 3) {
    return [];
  }

  const center = areaCenter(usableNodes);
  const projected = usableNodes
    .map((node) => ({
      ...projectedPoint(node, center),
      lat: node.lat,
      lng: node.lng
    }))
    .sort((first, second) => first.x === second.x ? first.y - second.y : first.x - second.x)
    .filter((point, index, list) => (
      index === 0 || point.x !== list[index - 1].x || point.y !== list[index - 1].y
    ));

  if (projected.length < 3) {
    return [];
  }

  const lower = [];
  projected.forEach((point) => {
    while (lower.length >= 2 && cross(lower[lower.length - 2], lower[lower.length - 1], point) <= 0) {
      lower.pop();
    }
    lower.push(point);
  });

  const upper = [];
  [...projected].reverse().forEach((point) => {
    while (upper.length >= 2 && cross(upper[upper.length - 2], upper[upper.length - 1], point) <= 0) {
      upper.pop();
    }
    upper.push(point);
  });

  return lower
    .slice(0, -1)
    .concat(upper.slice(0, -1))
    .map((point) => [point.lat, point.lng]);
}

export function sortedMembersForPath(members) {
  return [...members].sort((first, second) => {
    if (Math.abs(first.lng - second.lng) > Math.abs(first.lat - second.lat)) {
      return first.lng - second.lng;
    }

    return first.lat - second.lat;
  });
}

export function lineEndpoints(feature) {
  if (feature.geometry.length < 2) {
    return feature.geometry;
  }

  return [feature.geometry[0], feature.geometry[feature.geometry.length - 1]];
}

export function closestEndpointDistance(first, second) {
  const firstEndpoints = lineEndpoints(first);
  const secondEndpoints = lineEndpoints(second);
  let closest = Infinity;

  firstEndpoints.forEach((firstPoint) => {
    secondEndpoints.forEach((secondPoint) => {
      closest = Math.min(closest, distanceMeters(firstPoint, secondPoint));
    });
  });

  return closest;
}
