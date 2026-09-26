#!/usr/bin/env node
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const registry = read('main/ai/ai_capabilities.cc');
const tools = read('main/display/agent_ui/core/app_mcp_tools.cc');

// These source-level assertions protect the contract that can run without an
// ESP-IDF host toolchain. Behavioural tests use the same public registry API.
assert.match(registry, /kMaxPendingOperations = 16/);
assert.match(registry, /kMaxJsonDepth = 16/);
assert.match(registry, /JsonDepthWithinLimit/);
assert.match(registry, /IsObjectJson\(request\.arguments_json\)/);
assert.match(registry, /stale capability request/);
assert.match(registry, /pending operation has no result\/cancel handler/);
assert.match(registry, /bool CapabilityRegistry::Cancel/);
assert.match(registry, /bool CapabilityRegistry::Unregister/);
assert.match(registry, /request_id conflicts with different capability arguments/);
assert.match(registry, /cJSON_ParseWithLengthOpts/);
for (const name of ['self.capabilities.list', 'self.capabilities.describe',
                    'self.capabilities.invoke', 'self.capabilities.result',
                    'self.capabilities.cancel']) {
  assert.match(tools, new RegExp(name.replaceAll('.', '\\.')));
}
assert.match(tools, /IsSdExportedToHost\(\) \|\| disk\.IsBusy\(\)/);
assert.match(tools, /"system\.navigation"/);
console.log('ai capability registry/tool contract assertions passed');
