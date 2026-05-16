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

function findSession(name) {
  const sessions = loadSessions();

  if (!name && sessions.length === 1) return sessions[0];
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
    socket.setTimeout(10000);
    socket.on("data", (data) => {
      buffer += data.toString("utf8");
      const newline = buffer.indexOf("\n");
      if (newline < 0) return;

      socket.end();
      const response = JSON.parse(buffer.slice(0, newline));

      if (!response.ok) {
        reject(new Error(response.error?.message || "melatonin automation error"));
      } else {
        resolve(response.result);
      }
    });
    socket.on("timeout", () => reject(new Error("Timed out waiting for melatonin automation endpoint")));
    socket.on("error", reject);
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
      content: [{ type: "text", text: JSON.stringify(loadSessions(), null, 2) }],
    };
  }

  const session = findSession(args.session);
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

  const result = await endpointRequest(session, map[name], args);

  if (name === "juce_screenshot") {
    const content = [];
    if (result.file) content.push({ type: "text", text: result.file });
    if (result.base64) content.push({ type: "image", data: result.base64, mimeType: result.mimeType || "image/png" });
    return { content };
  }

  return {
    content: [{ type: "text", text: result.text || JSON.stringify(result, null, 2) }],
  };
}

const rl = readline.createInterface({ input: process.stdin });

rl.on("line", async (line) => {
  if (!line.trim()) return;

  const request = JSON.parse(line);

  try {
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
    send({ jsonrpc: "2.0", id: request.id, error: { code: -32000, message: error.message } });
  }
});
