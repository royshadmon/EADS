export const ANYLOG_USER_AGENT = "AnyLog/1.23";
export const OPERATOR_HEALTH_COMMAND = "test network with operator";

export const DEFAULT_OPERATOR_ADDRESSES = {
  operator1: "127.0.0.1:32148",
  operator2: "127.0.0.1:32248",
  operator3: "127.0.0.1:32348"
};

export const DEFAULT_OPERATOR_POSITIONS = {
  operator1: { lat: 36.9916, lng: -122.0583, location: "UC Santa Cruz" },
  operator2: { lat: 36.9741, lng: -122.0308, location: "Downtown Santa Cruz" },
  operator3: { lat: 36.9518, lng: -122.0577, location: "Natural Bridges State Beach" }
};

export async function fetchOperatorHealthRaw(options = {}) {
  const endpoint = options.endpoint || "/anylog";
  const timeoutMs = options.timeoutMs || 6000;
  const fetchImpl = options.fetchImpl || globalThis.fetch;

  if (!fetchImpl) {
    throw new Error("fetch is not available; pass options.fetchImpl for this runtime");
  }

  const controller = new AbortController();
  const timeoutId = globalThis.setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetchImpl(endpoint, {
      method: "GET",
      signal: controller.signal,
      headers: {
        "User-Agent": ANYLOG_USER_AGENT,
        command: OPERATOR_HEALTH_COMMAND,
        ...(options.headers || {})
      }
    });
    const text = await response.text();

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${text || response.statusText}`);
    }

    return text;
  } finally {
    globalThis.clearTimeout(timeoutId);
  }
}

export function parseOperatorTable(text, options = {}) {
  const operatorPositions = options.operatorPositions || DEFAULT_OPERATOR_POSITIONS;
  const operatorAddresses = options.operatorAddresses || DEFAULT_OPERATOR_ADDRESSES;
  const includeMissingOperators = options.includeMissingOperators !== false;
  const operators = [];

  for (const rawLine of String(text || "").split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("Address") || line.startsWith("-")) {
      continue;
    }

    const parts = line.replace(/^\|/, "").replace(/\|$/, "").split("|")
      .map((part) => part.trim());
    if (parts.length !== 4 || parts[1] !== "operator") {
      continue;
    }

    const [address, nodeType, name, status] = parts;
    const position = operatorPositions[name] || { lat: null, lng: null, location: "" };

    operators.push({
      name,
      address,
      type: nodeType,
      up: status === "+",
      status: status === "+" ? "up" : "down",
      ...position
    });
  }

  if (!includeMissingOperators) {
    return operators;
  }

  for (const [name, position] of Object.entries(operatorPositions)) {
    if (!operators.some((operator) => operator.name === name)) {
      operators.push({
        name,
        address: operatorAddresses[name] || "unknown",
        type: "operator",
        up: false,
        status: "down",
        ...position
      });
    }
  }

  return operators;
}

export async function fetchOperatorHealth(options = {}) {
  const raw = await fetchOperatorHealthRaw(options);

  return {
    queryNode: options.endpoint || "/anylog",
    command: OPERATOR_HEALTH_COMMAND,
    raw,
    operators: parseOperatorTable(raw, options)
  };
}

export function operatorNameForNode(node, index) {
  return node.operatorName || node.healthOperator || `operator${index + 1}`;
}

export function findOperatorForNode(node, index, operators) {
  const operatorName = operatorNameForNode(node, index);

  return operators.find((operator) => operator.name === operatorName)
    || operators.find((operator) => operator.address === node.operatorAddress)
    || operators.find((operator) => operator.address === node.address)
    || null;
}

export function normalizeNodesWithOperatorHealth(nodes, operators, options = {}) {
  const now = options.now || Date.now();
  const previousNodesById = new Map((options.previousNodes || [])
    .map((node) => [node.id || node.name, node]));

  return nodes.map((node, index) => {
    const previous = previousNodesById.get(node.id || node.name) || {};
    const operatorName = operatorNameForNode(node, index);
    const operator = findOperatorForNode(node, index, operators);
    const online = Boolean(operator?.up);
    const previousDownSince = node.downSince || previous.downSince || previous.anomalyStart;
    const downSince = online ? null : (previousDownSince || now);

    return {
      ...node,
      operatorName,
      operatorAddress: operator?.address || node.operatorAddress || node.address || null,
      online,
      up: online,
      status: online ? "normal" : "down",
      downSince,
      anomalyStart: online ? null : (node.anomalyStart || previous.anomalyStart || downSince),
      healthSource: OPERATOR_HEALTH_COMMAND,
      rawOperatorStatus: operator?.status || "down"
    };
  });
}

export function markNodesDownFromOperatorHealthError(nodes, options = {}) {
  const now = options.now || Date.now();
  const previousNodesById = new Map((options.previousNodes || [])
    .map((node) => [node.id || node.name, node]));

  return nodes.map((node, index) => {
    const previous = previousNodesById.get(node.id || node.name) || {};
    const previousDownSince = node.downSince || previous.downSince || previous.anomalyStart;
    const downSince = previousDownSince || now;

    return {
      ...node,
      operatorName: operatorNameForNode(node, index),
      operatorAddress: node.operatorAddress || node.address || null,
      online: false,
      up: false,
      status: "down",
      downSince,
      anomalyStart: node.anomalyStart || previous.anomalyStart || downSince,
      healthSource: OPERATOR_HEALTH_COMMAND,
      rawOperatorStatus: "down"
    };
  });
}
