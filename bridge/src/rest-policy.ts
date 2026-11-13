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
}

export function parseBridgeArgs(argv: string[]): BridgeArgs {
  const out: BridgeArgs = { setup: false };
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
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  return out;
}

export class RestCallError extends Error {
  readonly outcome = "unknown";
  readonly retrySafe = false;
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
  ) {
    super(message);
    this.name = "RestCallError";
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
  const body = error.responseBody;
  return (
    error.httpStatus === 404 &&
    typeof body === "object" &&
    body !== null &&
    "error" in body &&
    typeof body.error === "string" &&
    /unknown tool/i.test(body.error)
  );
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
    throw new RestCallError(
      `devbench returned ${response.status}; native outcome unknown; do not automatically replay`,
      "REST_HTTP_ERROR",
      deadline,
      response.status,
      body,
    );
  }
  return body;
}
