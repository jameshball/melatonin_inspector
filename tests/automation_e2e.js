#!/usr/bin/env node

const childProcess = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const sessionName = "automation_fixture";
const repoRoot = path.resolve(__dirname, "..");
const buildDir = process.env.MELATONIN_AUTOMATION_BUILD_DIR || path.join(os.tmpdir(), "melatonin-inspector-fixture");
const appPath = process.env.MELATONIN_AUTOMATION_APP || defaultAppPath(buildDir);
const cliPath = process.env.MELATONIN_UI || defaultCliPath(buildDir);
const mcpPath = process.env.MELATONIN_MCP || path.join(repoRoot, "tools", "melatonin_mcp.js");
const screenshotDir = process.env.MELATONIN_SCREENSHOT_DIR || os.tmpdir();

let appProcess;

main().catch((error) => {
  console.error(error.stack || error.message);
  process.exitCode = 1;
}).finally(async () => {
  await stopApp();
});

async function main() {
  assertFile(appPath, "automation fixture app");
  assertFile(cliPath, "melatonin-ui CLI");
  assertFile(mcpPath, "melatonin MCP server");

  cleanupSessionFiles();
  appProcess = childProcess.spawn(appPath, [], { stdio: "ignore" });

  await waitForSession();

  const listOutput = runCli(["list"]);
  assert(listOutput.includes(sessionName), "CLI list did not show automation_fixture");

  let snapshot = readSnapshot();
  assert(findByComponentName(snapshot.tree, "fixture.tabs"), "snapshot is missing top-level tabs");
  assert(findByComponentName(snapshot.tree, "controls.slider"), "snapshot is missing the controls slider");

  const rootPng = path.join(screenshotDir, "melatonin-automation-e2e-root.png");
  runCli(["-s", sessionName, "screenshot", "--target", "root", "--file", rootPng]);
  assertPng(rootPng, "root screenshot");

  clickRef(refByComponentName(snapshot.tree, "nav.editor"));
  snapshot = readSnapshot();
  assertStatus(snapshot, "Status: Editor");
  assert(findByComponentName(snapshot.tree, "editor.text"), "editor page did not expose its text editor");

  typeRef(refByComponentName(snapshot.tree, "editor.text"), "hello from automation");
  snapshot = readSnapshot();
  clickRef(refByComponentName(snapshot.tree, "editor.apply"));
  snapshot = readSnapshot();
  assertStatus(snapshot, "Status: Applied hello from automation");

  clickRef(refByComponentName(snapshot.tree, "nav.advanced"));
  snapshot = readSnapshot();
  assertStatus(snapshot, "Status: Advanced");
  assert(findByComponentName(snapshot.tree, "advanced.tabs"), "advanced page did not expose nested tabs");

  clickRef(refByComponentName(snapshot.tree, "advanced.goActions"));
  snapshot = readSnapshot();
  assertStatus(snapshot, "Status: Nested Actions");
  assert(findByComponentName(snapshot.tree, "advanced.reset"), "nested Actions tab did not expose Reset All");

  const resetRef = refByComponentName(snapshot.tree, "advanced.reset");
  runCli(["-s", sessionName, "set-bounds", resetRef, "--x", "20", "--y", "24", "--w", "180", "--h", "34"]);
  snapshot = readSnapshot();
  const resetAfterBounds = findByComponentName(snapshot.tree, "advanced.reset");
  assert(resetAfterBounds.bounds.w === 180 && resetAfterBounds.bounds.h === 34, "set-bounds did not update Reset All dimensions");

  runCli(["-s", sessionName, "set-property", resetAfterBounds.ref, "alpha", "0.9"]);
  clickRef(resetAfterBounds.ref);
  snapshot = readSnapshot();
  assertStatus(snapshot, "Status: Reset");

  await runMcpChecks();

  console.log(`ok - CLI and MCP automation e2e passed`);
  console.log(`root screenshot: ${rootPng}`);
}

function defaultAppPath(dir) {
  if (process.platform === "darwin") {
    return path.join(dir, "automation_fixture_artefacts", "automation_fixture.app", "Contents", "MacOS", "automation_fixture");
  }

  return path.join(dir, "automation_fixture_artefacts", exe("automation_fixture"));
}

function defaultCliPath(dir) {
  return path.join(dir, "melatonin-ui_artefacts", exe("melatonin-ui"));
}

function exe(name) {
  return process.platform === "win32" ? `${name}.exe` : name;
}

function runCli(args) {
  const result = childProcess.spawnSync(cliPath, args, { encoding: "utf8" });

  if (result.status !== 0) {
    throw new Error(`melatonin-ui ${args.join(" ")} failed\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  }

  return result.stdout;
}

function readSnapshot() {
  return JSON.parse(runCli(["-s", sessionName, "snapshot", "--format", "json", "--depth", "12"]));
}

function clickRef(ref) {
  runCli(["-s", sessionName, "click", ref]);
}

function typeRef(ref, text) {
  runCli(["-s", sessionName, "type", ref, text]);
}

async function waitForSession() {
  const deadline = Date.now() + 15000;

  while (Date.now() < deadline) {
    const listOutput = runCli(["list"]);
    if (listOutput.includes(sessionName)) {
      return;
    }

    await delay(250);
  }

  throw new Error(`Timed out waiting for ${sessionName} to advertise an automation session`);
}

async function runMcpChecks() {
  const mcp = new McpClient(mcpPath);

  try {
    await mcp.request("initialize", { protocolVersion: "2025-11-25", capabilities: {}, clientInfo: { name: "automation-e2e", version: "1" } });

    const tools = await mcp.request("tools/list", {});
    assert(tools.tools.some((tool) => tool.name === "juce_snapshot"), "MCP tools/list did not include juce_snapshot");
    assert(tools.tools.some((tool) => tool.name === "juce_screenshot"), "MCP tools/list did not include juce_screenshot");

    const snapshotResult = await mcp.request("tools/call", {
      name: "juce_snapshot",
      arguments: { session: sessionName, format: "text", depth: 12 },
    });
    assert(snapshotResult.content[0].text.includes("Automation Fixture Root"), "MCP snapshot did not include the fixture root");

    const mcpPng = path.join(screenshotDir, "melatonin-automation-e2e-mcp.png");
    const screenshotResult = await mcp.request("tools/call", {
      name: "juce_screenshot",
      arguments: { session: sessionName, target: "root", file: mcpPng },
    });

    assertPng(mcpPng, "MCP screenshot");
    assert(screenshotResult.content.some((item) => item.type === "image" && item.mimeType === "image/png" && item.data.length > 100), "MCP screenshot did not return image content");
  } finally {
    mcp.close();
  }
}

class McpClient {
  constructor(scriptPath) {
    this.child = childProcess.spawn(process.execPath, [scriptPath], { stdio: ["pipe", "pipe", "pipe"] });
    this.nextId = 1;
    this.pending = new Map();
    this.buffer = "";
    this.stderr = "";

    this.child.stdout.on("data", (data) => this.onData(data));
    this.child.stderr.on("data", (data) => {
      this.stderr += data.toString("utf8");
    });
    this.child.on("exit", (code) => {
      for (const { reject } of this.pending.values()) {
        reject(new Error(`MCP process exited with code ${code}\n${this.stderr}`));
      }
      this.pending.clear();
    });
  }

  request(method, params) {
    const id = this.nextId++;
    const message = { jsonrpc: "2.0", id, method, params };

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Timed out waiting for MCP ${method}\n${this.stderr}`));
      }, 10000);

      this.pending.set(id, { resolve, reject, timeout });
      this.child.stdin.write(JSON.stringify(message) + "\n");
    });
  }

  onData(data) {
    this.buffer += data.toString("utf8");

    for (;;) {
      const newline = this.buffer.indexOf("\n");
      if (newline < 0) {
        return;
      }

      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);

      if (!line.trim()) {
        continue;
      }

      const response = JSON.parse(line);
      const pending = this.pending.get(response.id);

      if (!pending) {
        continue;
      }

      clearTimeout(pending.timeout);
      this.pending.delete(response.id);

      if (response.error) {
        pending.reject(new Error(response.error.message));
      } else {
        pending.resolve(response.result);
      }
    }
  }

  close() {
    this.child.kill();
  }
}

function findByComponentName(tree, componentName) {
  return findNode(tree, (node) => node.componentName === componentName);
}

function refByComponentName(tree, componentName) {
  const node = findByComponentName(tree, componentName);
  assert(node, `Could not find componentName ${componentName}`);
  return node.ref;
}

function findByName(tree, name) {
  return findNode(tree, (node) => node.name === name || node.name === `Label: ${name}` || node.name.includes(name));
}

function refByName(tree, name) {
  const node = findByName(tree, name);
  assert(node, `Could not find node named ${name}`);
  return node.ref;
}

function findNode(node, predicate) {
  if (predicate(node)) {
    return node;
  }

  for (const child of node.children || []) {
    const found = findNode(child, predicate);
    if (found) {
      return found;
    }
  }

  return undefined;
}

function assertStatus(snapshot, expected) {
  const status = findByComponentName(snapshot.tree, "fixture.status");
  assert(status, "snapshot is missing fixture.status");
  const actual = [status.name, status.title, status.value].filter(Boolean).join("\n");
  assert(actual.includes(expected), `expected status "${expected}", got "${actual}"`);
}

function assertPng(file, label) {
  const bytes = fs.readFileSync(file);
  const pngSignature = "89504e470d0a1a0a";
  assert(bytes.subarray(0, 8).toString("hex") === pngSignature, `${label} is not a PNG: ${file}`);
  assert(bytes.length > 1000, `${label} is unexpectedly small: ${bytes.length} bytes`);
}

function cleanupSessionFiles() {
  const dir = path.join(os.tmpdir(), "melatonin_inspector", "sessions");

  try {
    for (const fileName of fs.readdirSync(dir)) {
      if (!fileName.endsWith(".json")) {
        continue;
      }

      const file = path.join(dir, fileName);
      const session = JSON.parse(fs.readFileSync(file, "utf8"));

      if (session.session === sessionName) {
        fs.unlinkSync(file);
      }
    }
  } catch {
  }
}

async function stopApp() {
  if (!appProcess || appProcess.killed) {
    return;
  }

  appProcess.kill("SIGTERM");
  await Promise.race([
    new Promise((resolve) => appProcess.once("exit", resolve)),
    delay(2000).then(() => appProcess.kill("SIGKILL")),
  ]);
}

function assertFile(file, label) {
  assert(fs.existsSync(file), `Missing ${label}: ${file}`);
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
