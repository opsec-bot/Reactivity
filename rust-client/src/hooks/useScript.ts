import { useCallback, useEffect, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { api, type ScriptAction, type ScriptStatus } from "@/lib/api";
import { SCRIPT_EXAMPLES } from "@/lib/scriptExamples";

const DRAFT_KEY = "superlight.script.draft";
const MAX_OUTPUT = 20_000;

function loadDraft(): string {
  try {
    return localStorage.getItem(DRAFT_KEY) ?? SCRIPT_EXAMPLES[0].source;
  } catch {
    return SCRIPT_EXAMPLES[0].source;
  }
}

export interface Upload {
  sent: number;
  total: number;
}

/** The on-board Lua script: editor draft, upload progress, board state and output. */
export function useScript({ connected }: { connected: boolean }) {
  const [source, setSourceState] = useState(loadDraft);
  const [status, setStatus] = useState<ScriptStatus | null>(null);
  const [output, setOutput] = useState("");
  const [upload, setUpload] = useState<Upload | null>(null);
  const [error, setError] = useState<string | null>(null);
  const uploadRef = useRef<Upload | null>(null);

  const setSource = useCallback((s: string) => {
    setSourceState(s);
    try {
      localStorage.setItem(DRAFT_KEY, s);
    } catch {
      /* storage unavailable: the draft just isn't remembered */
    }
  }, []);

  const appendOutput = useCallback((text: string) => {
    setOutput((prev) => {
      const next = prev + text;
      return next.length > MAX_OUTPUT ? next.slice(next.length - MAX_OUTPUT) : next;
    });
  }, []);

  const finishUpload = useCallback(() => {
    uploadRef.current = null;
    setUpload(null);
  }, []);

  useEffect(() => {
    let alive = true;
    let un: (() => void) | undefined;
    (async () => {
      const u = await listen<string>("serial://line", (e) => {
        let msg: Record<string, unknown>;
        try {
          msg = JSON.parse(e.payload);
        } catch {
          return;
        }
        if (msg.type === "script_status") {
          setStatus(msg as unknown as ScriptStatus);
        } else if (msg.type === "script_log") {
          if (msg.clear) setOutput("");
          else if (typeof msg.text === "string") appendOutput(msg.text);
        } else if (msg.type === "ack" && msg.what === "script_chunk") {
          const up = uploadRef.current;
          if (up && typeof msg.n === "number") {
            uploadRef.current = { ...up, sent: msg.n };
            setUpload(uploadRef.current);
          }
        } else if (msg.type === "ack" && msg.what === "script_end") {
          finishUpload();
        } else if (
          msg.type === "error" &&
          typeof msg.what === "string" &&
          msg.what.startsWith("script_")
        ) {
          setError(`${msg.what}: ${String(msg.msg)}`);
          finishUpload();
        }
      });
      if (alive) un = u;
      else u();
    })();
    return () => {
      alive = false;
      un?.();
    };
  }, [appendOutput, finishUpload]);

  // Fresh snapshot on every (re)connect; forget the old one on disconnect.
  useEffect(() => {
    if (connected) {
      const t = setTimeout(() => api.scriptCommand("status").catch(() => {}), 300);
      return () => clearTimeout(t);
    }
    setStatus(null);
    finishUpload();
  }, [connected, finishUpload]);

  const guard = useCallback(async (fn: () => Promise<void>) => {
    setError(null);
    try {
      await fn();
    } catch (e) {
      setError(String(e));
      finishUpload();
    }
  }, [finishUpload]);

  const uploadAndRun = useCallback(async () => {
    const total = new TextEncoder().encode(source).length;
    uploadRef.current = { sent: 0, total };
    setUpload(uploadRef.current);
    await api.scriptUpload(source, true);
  }, [source]);

  /** Upload the editor's script and start it (replaces the running one). */
  const run = useCallback(() => guard(uploadAndRun), [guard, uploadAndRun]);

  /** Upload, start, and store in flash so it runs at every boot. */
  const saveToBoard = useCallback(
    () =>
      guard(async () => {
        await uploadAndRun();
        await api.scriptCommand("save");
      }),
    [guard, uploadAndRun],
  );

  const command = useCallback(
    (action: ScriptAction) => guard(() => api.scriptCommand(action)),
    [guard],
  );

  return {
    source,
    setSource,
    status,
    output,
    clearOutput: () => setOutput(""),
    upload,
    error,
    run,
    saveToBoard,
    stop: () => command("stop"),
    erase: () => command("erase"),
  };
}

export type ScriptCtl = ReturnType<typeof useScript>;
