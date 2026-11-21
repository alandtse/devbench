// Tool-call policy only. No native request splitting, retries, or client config writes.
export const DEFAULT_REST_TIMEOUT_MS = 30_000;
export const MAX_REST_TIMEOUT_MS = 540_000;

export function validateRestTimeout(value: number): number {
  if (
    !Number.isSafeInteger(value) ||
    value < 1 ||
    value > MAX_REST_TIMEOUT_MS
  ) {
    throw new Error(
      `REST timeout must be an integer in 1..${MAX_REST_TIMEOUT_MS} ms`,
    );
  }
  return value;
}

export interface BridgeArgs {
  game?: string;
  install?: string;
  setup: boolean;
  restTimeoutMs?: number;
  unknown: string[];
}

export function parseBridgeArgs(argv: string[]): BridgeArgs {
  const out: BridgeArgs = { setup: false, unknown: [] };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === "setup") {
      out.setup = true;
    } else if (
      arg === "--game" ||
      arg === "--install" ||
      arg === "--rest-timeout-ms"
    ) {
      const value = argv[++i];
      if (!value || value.startsWith("--"))
        throw new Error(`${arg} requires a value`);
      if (arg === "--game") out.game = value;
      else if (arg === "--install") out.install = value;
      else {
        if (out.restTimeoutMs !== undefined)
          throw new Error("duplicate --rest-timeout-ms");
        if (!/^[0-9]+$/.test(value))
          throw new Error("--rest-timeout-ms requires decimal milliseconds");
        out.restTimeoutMs = validateRestTimeout(Number(value));
      }
    } else {
      out.unknown.push(arg);
    }
  }
  return out;
}

export type RestOutcome = "unknown" | "rejected";

// Thrown when the request never reached devbench (nothing listening on the port).
export class GameUnavailableError extends Error {}

export class RestCallError extends Error {
  readonly outcome: RestOutcome;
  readonly retrySafe: boolean;
  readonly code:
    | "REST_TIMEOUT"
    | "REST_TRANSPORT_ERROR"
    | "REST_HTTP_ERROR"
    | "REST_INVALID_JSON";
  readonly timeoutMs: number;
  readonly httpStatus?: number;
  readonly responseBody?: unknown;

  constructor(
    message: string,
    code:
      | "REST_TIMEOUT"
      | "REST_TRANSPORT_ERROR"
      | "REST_HTTP_ERROR"
      | "REST_INVALID_JSON",
    timeoutMs: number,
    httpStatus?: number,
    responseBody?: unknown,
    outcome: RestOutcome = "unknown",
  ) {
    super(message);
    this.name = "RestCallError";
    this.outcome = outcome;
    this.retrySafe = outcome === "rejected";
    this.code = code;
    this.timeoutMs = timeoutMs;
    this.httpStatus = httpStatus;
    this.responseBody = responseBody;
  }

  details(): Record<string, unknown> {
    return {
      code: this.code,
      outcome: this.outcome,
      retrySafe: this.retrySafe,
      timeoutMs: this.timeoutMs,
      ...(this.httpStatus !== undefined ? { httpStatus: this.httpStatus } : {}),
      ...(this.responseBody !== undefined
        ? { responseBody: this.responseBody }
        : {}),
    };
  }
}

export function isUnknownToolError(error: unknown): boolean {
  if (!(error instanceof RestCallError)) {
    return error instanceof Error && /unknown tool/i.test(error.message);
  }
  return (
    error.httpStatus !== undefined &&
    isPreHandlerRejection(error.httpStatus, error.responseBody)
  );
}

const CONNECT_FAILURE_CODES = new Set([
  "ECONNREFUSED",
  "ENOTFOUND",
  "EHOSTUNREACH",
  "ENETUNREACH",
  "ConnectionRefused",
  "FailedToOpenSocket",
]);

// True only when the connection was never established, so no request bytes were sent.
function isConnectFailure(error: unknown): boolean {
  for (let e: unknown = error, depth = 0; e && depth < 4; depth++) {
    const code = (e as { code?: unknown }).code;
    if (typeof code === "string" && CONNECT_FAILURE_CODES.has(code))
      return true;
    e = (e as { cause?: unknown }).cause;
  }
  return false;
}

// Only the registry's unknown-tool 404 is answered before a handler runs. Other 4xx can follow
// partial work (e.g. a 409 after the engine cursor moved), so they stay uncertain.
function isPreHandlerRejection(status: number, body: unknown): boolean {
  const message = (body as { error?: unknown } | null)?.error;
  return (
    status === 404 &&
    typeof message === "string" &&
    /unknown tool/i.test(message)
  );
}

function serverMessage(body: unknown, fallback: string): string {
  const message = (body as { error?: unknown } | null)?.error;
  return typeof message === "string" && message
    ? message
    : fallback || "no detail";
}

// The one deadline covers both response headers and the complete body. A timeout
// cancels only our fetch; it does NOT establish native cancellation or no effect.
// Never follow Location: even one fetch can otherwise resend/transform a POST.
export async function fetchToolJson(
  url: string,
  init: RequestInit,
  timeoutMs = DEFAULT_REST_TIMEOUT_MS,
): Promise<unknown> {
  const deadline = validateRestTimeout(timeoutMs);
  const signal = AbortSignal.timeout(deadline);
  let response: Response;
  let text: string;
  try {
    response = await fetch(url, { ...init, signal, redirect: "manual" });
    text = await response.text();
  } catch (error) {
    const timeout = signal.aborted;
    if (!timeout && isConnectFailure(error)) {
      throw new GameUnavailableError(
        `devbench not reachable at ${url}: ${String(error)}`,
      );
    }
    throw new RestCallError(
      `${timeout ? "REST deadline exceeded" : "REST transport failed"} (${deadline}ms): ${String(error)}; native outcome unknown; do not automatically replay`,
      timeout ? "REST_TIMEOUT" : "REST_TRANSPORT_ERROR",
      deadline,
    );
  }
  let body: unknown;
  try {
    body = JSON.parse(text) as unknown;
  } catch {
    throw new RestCallError(
      `devbench returned ${response.status} with a non-JSON body; native outcome unknown; do not automatically replay`,
      response.ok ? "REST_INVALID_JSON" : "REST_HTTP_ERROR",
      deadline,
      response.status,
      text,
    );
  }
  if (!response.ok) {
    const rejected = isPreHandlerRejection(response.status, body);
    const detail = serverMessage(body, response.statusText);
    throw new RestCallError(
      rejected
        ? `devbench returned ${response.status}: ${detail}`
        : `devbench returned ${response.status}: ${detail}; native outcome unknown; do not automatically replay`,
      "REST_HTTP_ERROR",
      deadline,
      response.status,
      body,
      rejected ? "rejected" : "unknown",
    );
  }
  return body;
}
