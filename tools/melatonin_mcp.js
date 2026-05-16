#!/usr/bin/env node

const fs = require("fs");
const net = require("net");
const os = require("os");
const path = require("path");
const readline = require("readline");

function sessionsDir() {
  return path.join(os.tmpdir(), "melatonin_inspector", "sessions");
}

function loadSessions() {
  try {
    return fs
      .readdirSync(sessionsDir())
      .filter((name) => name.endsWith(".json"))
      .map((name) => {
        const file = path.join(sessionsDir(), name);
        return { ...JSON.parse(fs.readFileSync(file, "utf8")), file, modifiedAtMs: fs.statSync(file).mtimeMs };
      });
  } catch {
    return [];
  }
}

function isSessionReachable(session, timeoutMs = 250) {
  return new Promise((resolve) => {
    if (!session.host || !session.port) {
      resolve(false);
      return;
    }

    const socket = net.createConnection({ host: session.host, port: session.port });
    let settled = false;

    function finish(result) {
      if (settled) return;
      settled = true;
      socket.destroy();
      resolve(result);
    }

    socket.setTimeout(timeoutMs);
    socket.on("connect", () => finish(true));
    socket.on("timeout", () => finish(false));
    socket.on("error", () => finish(false));
  });
}

async function liveSessions() {
  const checked = await Promise.all(
    loadSessions().map(async (session) => ((await isSessionReachable(session)) ? session : undefined))
  );

  return checked.filter(Boolean);
}

async function findSession(name) {
  const sessions = await liveSessions();

  if (!name && sessions.length === 1) return sessions[0];
  if (!name) return undefined;

  return sessions
    .filter((s) => s.session === name || s.file.includes(name))
    .sort((a, b) => b.modifiedAtMs - a.modifiedAtMs)[0];
}

function endpointRequest(session, method, params = {}) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: session.host, port: session.port }, () => {
      socket.write(JSON.stringify({ id: "1", token: session.token, method, params }) + "\n");
    });

    let buffer = "";
    let settled = false;

    function finish(error, result) {
      if (settled) return;
      settled = true;
      socket.destroy();

      if (error) reject(error);
      else resolve(result);
    }

    socket.setTimeout(10000);
    socket.on("data", (data) => {
      buffer += data.toString("utf8");
      const newline = buffer.indexOf("\n");
      if (newline < 0) return;

      let response;

      try {
        response = JSON.parse(buffer.slice(0, newline));
      } catch (error) {
        finish(error);
        return;
      }

      if (!response.ok) {
        finish(new Error(response.error?.message || "melatonin automation error"));
      } else {
        finish(undefined, response.result);
      }
    });
    socket.on("timeout", () => finish(new Error("Timed out waiting for melatonin automation endpoint")));
    socket.on("error", finish);
  });
}

const tools = [
  {
    name: "juce_list_sessions",
    description: "List running melatonin_inspector automation sessions.",
    inputSchema: { type: "object", properties: {} },
  },
  {
    name: "juce_snapshot",
    description: "Return a compact Playwright-style snapshot of a JUCE component tree.",
    inputSchema: {
      type: "object",
      properties: {
        session: { type: "string" },
        format: { type: "string", enum: ["text", "json"], default: "text" },
        depth: { type: "number", default: 8 },
      },
    },
  },
  {
    name: "juce_screenshot",
    description: "Capture a PNG screenshot of the root or a component ref.",
    inputSchema: {
      type: "object",
      properties: {
        session: { type: "string" },
        target: { type: "string", default: "root" },
        ref: { type: "string" },
        file: { type: "string" },
      },
    },
  },
  {
    name: "juce_click",
    description: "Click a component ref and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, ref: { type: "string" } }, required: ["ref"] },
  },
  {
    name: "juce_click_xy",
    description: "Click root-local coordinates and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, x: { type: "number" }, y: { type: "number" } }, required: ["x", "y"] },
  },
  {
    name: "juce_type",
    description: "Type text into a component ref and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, ref: { type: "string" }, text: { type: "string" } }, required: ["ref", "text"] },
  },
  {
    name: "juce_press",
    description: "Press a key and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, ref: { type: "string" }, key: { type: "string" } }, required: ["key"] },
  },
  {
    name: "juce_drag",
    description: "Drag a component by a delta and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, ref: { type: "string" }, dx: { type: "number" }, dy: { type: "number" } }, required: ["ref"] },
  },
  {
    name: "juce_set_bounds",
    description: "Set a component's bounds and return a fresh snapshot.",
    inputSchema: {
      type: "object",
      properties: { session: { type: "string" }, ref: { type: "string" }, x: { type: "number" }, y: { type: "number" }, w: { type: "number" }, h: { type: "number" } },
      required: ["ref"],
    },
  },
  {
    name: "juce_set_property",
    description: "Set a component property and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, ref: { type: "string" }, name: { type: "string" }, value: {} }, required: ["ref", "name"] },
  },
  {
    name: "juce_wait",
    description: "Wait briefly and return a fresh snapshot.",
    inputSchema: { type: "object", properties: { session: { type: "string" }, ms: { type: "number", default: 250 } } },
  },
];

function send(message) {
  process.stdout.write(JSON.stringify(message) + "\n");
}

async function callTool(name, args = {}) {
  if (name === "juce_list_sessions") {
    return {
      content: [{ type: "text", text: JSON.stringify(await liveSessions(), null, 2) }],
    };
  }

  const session = await findSession(args.session);
  if (!session) throw new Error(`No melatonin_inspector automation session found${args.session ? ` for '${args.session}'` : ""}`);

  const map = {
    juce_snapshot: "snapshot",
    juce_screenshot: "screenshot",
    juce_click: "click",
    juce_click_xy: "click_xy",
    juce_type: "type",
    juce_press: "press",
    juce_drag: "drag",
    juce_set_bounds: "set_bounds",
    juce_set_property: "set_property",
    juce_wait: "wait",
  };

  const method = map[name];
  if (!method) throw new Error(`Unknown melatonin MCP tool: ${name}`);

  const result = await endpointRequest(session, method, args);

  if (name === "juce_screenshot") {
    const content = [];
    if (result.file) content.push({ type: "text", text: result.file });
    if (result.base64) content.push({ type: "image", data: result.base64, mimeType: result.mimeType || "image/png" });
    return { content };
  }

  if (name === "juce_snapshot" && args.format === "json") {
    return {
      content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
    };
  }

  return {
    content: [{ type: "text", text: result.text || JSON.stringify(result, null, 2) }],
  };
}

const rl = readline.createInterface({ input: process.stdin });

rl.on("line", async (line) => {
  if (!line.trim()) return;

  let request;

  try {
    request = JSON.parse(line);

    if (request.method === "initialize") {
      send({
        jsonrpc: "2.0",
        id: request.id,
        result: {
          protocolVersion: request.params?.protocolVersion || "2025-11-25",
          capabilities: { tools: {} },
          serverInfo: { name: "melatonin-mcp", version: "0.1.0" },
        },
      });
      return;
    }

    if (request.method === "tools/list") {
      send({ jsonrpc: "2.0", id: request.id, result: { tools } });
      return;
    }

    if (request.method === "tools/call") {
      const result = await callTool(request.params.name, request.params.arguments || {});
      send({ jsonrpc: "2.0", id: request.id, result });
      return;
    }

    if (request.id !== undefined) send({ jsonrpc: "2.0", id: request.id, result: {} });
  } catch (error) {
    send({ jsonrpc: "2.0", id: request?.id ?? null, error: { code: -32000, message: error.message } });
  }
});
