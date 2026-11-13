import assert from "node:assert/strict";
import { createServer } from "node:http";
import { once } from "node:events";
import { test } from "node:test";
import { registerHooks } from "node:module";
import { mkdtemp, writeFile, unlink, rmdir } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import {
  parseBridgeArgs,
  validateRestTimeout,
  fetchToolJson,
  RestCallError,
  isUnknownToolError,
  DEFAULT_REST_TIMEOUT_MS,
  MAX_REST_TIMEOUT_MS,
} from "../src/rest-policy.ts";

// Node strips the real production TS; only its two .js import specifiers need
// mapping to their exact TS sources. No production code or HTTP is replaced.
// Scope the hook to proxy.ts and remove it immediately after module loading.
async function sourceProxy() {
  const proxyUrl = new URL("../src/proxy.ts", import.meta.url).href;
  const loader = registerHooks({
    resolve(specifier, context, nextResolve) {
      if (
        context.parentURL === proxyUrl &&
        (specifier === "./runtime.js" || specifier === "./rest-policy.js")
      ) {
        return nextResolve(
          new URL(specifier.replace(/\.js$/, ".ts"), proxyUrl).href,
          context,
        );
      }
      return nextResolve(specifier, context);
    },
  });
  try {
    return await import(proxyUrl);
  } finally {
    loader.deregister();
  }
}
const { callTool } = await sourceProxy();

test("a structured unknown-tool 404 still triggers catalogue refresh", async () => {
  await fixture(
    (req, res) => {
      req.resume();
      res.writeHead(404, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          error: "unknown tool 'gone' (not registered in this game)",
          code: 404,
        }),
      );
    },
    (url) =>
      targetFixture(url, async (target) => {
        await assert.rejects(callTool(target, "gone", {}), (error) => {
          assert.equal(error.retrySafe, false);
          assert.equal(isUnknownToolError(error), true);
          return true;
        });
      }),
  );
  assert.equal(
    isUnknownToolError(
      new RestCallError("REST HTTP error", "REST_HTTP_ERROR", 30_000, 404, {
        error: "reference not found",
      }),
    ),
    false,
  );
});

async function targetFixture(url, run) {
  const dir = await mkdtemp(join(tmpdir(), "devbench-redirect-owned-"));
  const file = join(dir, "runtime.json");
  try {
    await writeFile(file, JSON.stringify({ port: Number(new URL(url).port) }));
    await run({ label: "owned-loopback", runtimeDir: dir });
  } finally {
    await unlink(file).catch((error) => {
      if (error.code !== "ENOENT") throw error;
    });
    await rmdir(dir);
  }
}

async function fixture(handler, run) {
  const server = createServer(handler);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  try {
    await run(`http://127.0.0.1:${server.address().port}`);
  } finally {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  }
}

test("rest policy: defaults and explicit long-call envelopes remain bounded", () => {
  assert.equal(DEFAULT_REST_TIMEOUT_MS, 30000);
  assert.deepEqual(parseBridgeArgs(["--game", "vr"]), {
    setup: false,
    game: "vr",
  });
  for (const [native, bridge, client] of [
    [120000, 124000, 125000],
    [60000, 64000, 65000],
    [30000, 34000, 35000],
  ]) {
    const parsed = parseBridgeArgs([
      "--game",
      "vr",
      "--rest-timeout-ms",
      String(bridge),
    ]);
    assert.equal(parsed.restTimeoutMs, bridge);
    assert.ok(native < bridge && bridge < client && client < 600000);
  }
  assert.equal(validateRestTimeout(MAX_REST_TIMEOUT_MS), 540000);
});

test("rest policy: invalid and duplicate CLI deadlines fail before any call", () => {
  for (const value of [
    "0",
    "-1",
    "540001",
    "NaN",
    "1.2",
    "1e5",
    "Infinity",
    "9007199254740993",
    "",
  ]) {
    assert.throws(() => parseBridgeArgs(["--rest-timeout-ms", value]));
  }
  assert.throws(() => parseBridgeArgs(["--rest-timeout-ms"]));
  assert.throws(() => parseBridgeArgs(["--rest-timeout-ms", "--game", "vr"]));
  assert.throws(() =>
    parseBridgeArgs([
      "--rest-timeout-ms",
      "30000",
      "--rest-timeout-ms",
      "124000",
    ]),
  );
  assert.throws(() => parseBridgeArgs(["--rest-timeot-ms", "124000"]));
});

test("rest policy: one unchanged POST and successful native JSON pass through", async () => {
  let calls = 0;
  const args = {
    operation: "qualification_wait",
    timeoutMs: 120000,
    nested: [null, false, "x"],
  };
  const response = {
    ok: false,
    reason: "native-declared",
    value: { typed: 1 },
  };
  await fixture(
    async (req, res) => {
      calls++;
      assert.equal(req.method, "POST");
      assert.equal(req.url, "/api/tool/qualification_wait");
      let body = "";
      for await (const chunk of req) body += chunk;
      assert.deepEqual(JSON.parse(body), args);
      res.end(JSON.stringify(response));
    },
    async (url) => {
      assert.deepEqual(
        await fetchToolJson(
          `${url}/api/tool/qualification_wait`,
          {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(args),
          },
          124000,
        ),
        response,
      );
    },
  );
  assert.equal(calls, 1);
});

test("rest policy: HTTP error preserves status and original JSON without retry", async () => {
  let calls = 0;
  const body = {
    error: "owned refusal",
    operationId: "fixture-only",
    retryAfter: 12,
  };
  await fixture(
    (_req, res) => {
      calls++;
      res.writeHead(503);
      res.end(JSON.stringify(body));
    },
    async (url) => {
      await assert.rejects(
        fetchToolJson(url, { method: "POST" }, 1000),
        (error) => {
          assert.ok(error instanceof RestCallError);
          assert.equal(error.code, "REST_HTTP_ERROR");
          assert.equal(error.httpStatus, 503);
          assert.deepEqual(error.responseBody, body);
          assert.equal(error.outcome, "unknown");
          assert.equal(error.retrySafe, false);
          return true;
        },
      );
    },
  );
  assert.equal(calls, 1);
});

test("rest policy: deadline before headers is uncertain and never replayed", async () => {
  let calls = 0;
  await fixture(
    (_req, _res) => {
      calls++;
    },
    async (url) => {
      await assert.rejects(
        fetchToolJson(url, { method: "POST" }, 100),
        (error) => {
          assert.equal(error.code, "REST_TIMEOUT");
          assert.equal(error.timeoutMs, 100);
          assert.equal(error.retrySafe, false);
          assert.match(error.message, /native outcome unknown/);
          assert.doesNotMatch(error.message, /game not running/);
          return true;
        },
      );
    },
  );
  assert.equal(calls, 1);
});

test("rest policy: deadline covers a stalled response body, not just headers", async () => {
  let calls = 0;
  await fixture(
    (_req, res) => {
      calls++;
      res.writeHead(200);
      res.flushHeaders();
      res.write('{"partial":');
    },
    async (url) => {
      await assert.rejects(
        fetchToolJson(url, { method: "POST" }, 150),
        (error) => {
          assert.equal(error.code, "REST_TIMEOUT");
          assert.equal(error.outcome, "unknown");
          return true;
        },
      );
    },
  );
  assert.equal(calls, 1);
});

test("rest policy: malformed response retains raw text and uncertain semantics", async () => {
  await fixture(
    (_req, res) => {
      res.end("not JSON");
    },
    async (url) => {
      await assert.rejects(
        fetchToolJson(url, { method: "POST" }, 1000),
        (error) => {
          assert.equal(error.code, "REST_INVALID_JSON");
          assert.equal(error.responseBody, "not JSON");
          assert.equal(error.httpStatus, 200);
          assert.equal(error.retrySafe, false);
          return true;
        },
      );
    },
  );
});

test("rest policy: explicit bridge duration controls owned delayed reply", async () => {
  let calls = 0;
  await fixture(
    (_req, res) => {
      calls++;
      const timer = setTimeout(() => res.end('{"completed":true}'), 200);
      res.on("close", () => clearTimeout(timer));
    },
    async (url) => {
      assert.deepEqual(await fetchToolJson(url, { method: "POST" }, 1500), {
        completed: true,
      });
    },
  );
  assert.equal(calls, 1);
});

test("rest policy: disconnected response is uncertain, not a missing game claim", async () => {
  let calls = 0;
  await fixture(
    (req) => {
      calls++;
      req.socket.destroy();
    },
    async (url) => {
      await assert.rejects(
        fetchToolJson(url, { method: "POST" }, 1000),
        (error) => {
          assert.equal(error.code, "REST_TRANSPORT_ERROR");
          assert.equal(error.retrySafe, false);
          assert.doesNotMatch(error.message, /game not running/);
          return true;
        },
      );
    },
  );
  assert.equal(calls, 1);
});

for (const status of [301, 302, 303, 307, 308]) {
  test(`rest policy: production callTool never follows ${status} or resends its POST body`, async () => {
    let calls = 0,
      redirected = 0;
    const args = {
      operation: "owned-mutation",
      nested: [null, false, "original"],
    };
    const refusal = { error: "owned redirect refusal", status };
    await fixture(
      (_req, res) => {
        redirected++;
        res.end('{"unexpected":"followed"}');
      },
      async (other) => {
        await fixture(
          async (req, res) => {
            calls++;
            assert.equal(req.method, "POST");
            assert.equal(req.url, "/api/tool/fixture%2Fmutation");
            let text = "";
            for await (const chunk of req) text += chunk;
            assert.equal(text, JSON.stringify(args));
            res.writeHead(status, {
              Location: `${other}/must-not-receive-body`,
            });
            res.end(JSON.stringify(refusal));
          },
          async (url) => {
            await targetFixture(url, async (target) => {
              await assert.rejects(
                callTool(target, "fixture/mutation", args, 1500),
                (error) => {
                  assert.ok(error instanceof RestCallError);
                  assert.deepEqual(error.details(), {
                    code: "REST_HTTP_ERROR",
                    outcome: "unknown",
                    retrySafe: false,
                    timeoutMs: 1500,
                    httpStatus: status,
                    responseBody: refusal,
                  });
                  return true;
                },
              );
            });
          },
        );
      },
    );
    assert.equal(calls, 1);
    assert.equal(redirected, 0); // no POST replay, transformed GET, or target qualification
  });
}

test("rest policy: caller redirect follow option cannot override the no-replay policy", async () => {
  let calls = 0,
    redirected = 0;
  await fixture(
    (_req, res) => {
      redirected++;
      res.end('{"unexpected":true}');
    },
    async (other) => {
      await fixture(
        (_req, res) => {
          calls++;
          res.writeHead(307, { Location: `${other}/not-called` });
          res.end("original non-JSON redirect body");
        },
        async (url) => {
          await assert.rejects(
            fetchToolJson(
              url,
              { method: "POST", body: "original", redirect: "follow" },
              1500,
            ),
            (error) => {
              assert.equal(error.code, "REST_HTTP_ERROR");
              assert.equal(error.httpStatus, 307);
              assert.equal(
                error.responseBody,
                "original non-JSON redirect body",
              );
              assert.equal(error.retrySafe, false);
              return true;
            },
          );
        },
      );
    },
  );
  assert.equal(calls, 1);
  assert.equal(redirected, 0);
});

test("rest policy: production redirect body retains the original finite deadline", async () => {
  let calls = 0,
    redirected = 0;
  await fixture(
    (_req, res) => {
      redirected++;
      res.end("{}");
    },
    async (other) => {
      await fixture(
        (_req, res) => {
          calls++;
          res.writeHead(308, { Location: `${other}/not-called` });
          res.flushHeaders();
          res.write('{"unfinished":');
        },
        async (url) => {
          await targetFixture(url, async (target) => {
            await assert.rejects(
              callTool(target, "fixture-deadline", {}, 150),
              (error) => {
                assert.equal(error.code, "REST_TIMEOUT");
                assert.equal(error.timeoutMs, 150);
                assert.equal(error.outcome, "unknown");
                assert.equal(error.retrySafe, false);
                return true;
              },
            );
          });
        },
      );
    },
  );
  assert.equal(calls, 1);
  assert.equal(redirected, 0);
});
