import { useEffect, useRef, type KeyboardEvent } from "react";
import { BookOpen, FileCode, HardDrive, Play, Square, Terminal, Trash2, Upload } from "lucide-react";
import { SCRIPT_MAX_LEN } from "@/lib/api";
import { SCRIPT_API, SCRIPT_EXAMPLES } from "@/lib/scriptExamples";
import type { ScriptCtl } from "@/hooks/useScript";
import { cn } from "@/lib/utils";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card, CardAction, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";

const STATE_LABEL = { idle: "stopped", loading: "loading", running: "running" } as const;

export function ScriptPanel({ s, connected }: { s: ScriptCtl; connected: boolean }) {
  const bytes = new TextEncoder().encode(s.source).length;
  const tooBig = bytes > SCRIPT_MAX_LEN;
  const busy = !!s.upload;
  const state = s.status?.state;
  const outRef = useRef<HTMLPreElement>(null);

  useEffect(() => {
    outRef.current?.scrollIntoView({ block: "end" });
  }, [s.output]);

  // Tab indents instead of leaving the editor; Ctrl/Cmd+Enter runs.
  const onKeyDown = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === "Tab") {
      e.preventDefault();
      const el = e.currentTarget;
      const { selectionStart: a, selectionEnd: b, value } = el;
      s.setSource(value.slice(0, a) + "  " + value.slice(b));
      requestAnimationFrame(() => el.setSelectionRange(a + 2, a + 2));
    } else if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      if (connected && !busy && !tooBig) s.run();
    }
  };

  return (
    <div className="grid gap-4">
      <Card>
        <CardHeader className="pb-3">
          <CardTitle className="flex items-center gap-2 text-sm">
            <FileCode className="size-4" /> Lua script
            {state && (
              <Badge
                variant={state === "running" ? "default" : "secondary"}
                className={cn("ml-1", state === "running" && "bg-success text-success-foreground")}
              >
                {STATE_LABEL[state]}
              </Badge>
            )}
            {s.status?.saved && (
              <Badge variant="outline" className="gap-1">
                <HardDrive className="size-3" /> saved on board
              </Badge>
            )}
          </CardTitle>
          <CardDescription>
            Runs on the Pico itself, so it keeps working with this app closed. Same functions as
            Logitech G Hub scripts.
          </CardDescription>
          <CardAction>
            <Select
              value=""
              onValueChange={(id) => {
                const ex = SCRIPT_EXAMPLES.find((x) => x.id === id);
                if (ex) s.setSource(ex.source);
              }}
            >
              <SelectTrigger className="h-8 w-52 text-xs">
                <SelectValue placeholder="Load an example…" />
              </SelectTrigger>
              <SelectContent>
                {SCRIPT_EXAMPLES.map((ex) => (
                  <SelectItem key={ex.id} value={ex.id} className="text-xs">
                    {ex.name}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </CardAction>
        </CardHeader>
        <CardContent className="space-y-3">
          <textarea
            value={s.source}
            onChange={(e) => s.setSource(e.target.value)}
            onKeyDown={onKeyDown}
            spellCheck={false}
            className="h-80 w-full resize-y rounded-md border bg-muted/30 p-3 font-mono text-xs leading-relaxed outline-none focus-visible:ring-2 focus-visible:ring-ring"
          />
          <div className="flex flex-wrap items-center gap-2">
            <Button className="gap-2" disabled={!connected || busy || tooBig} onClick={s.run} title="Ctrl+Enter">
              <Play className="size-4" /> Run on board
            </Button>
            <Button
              variant="secondary"
              className="gap-2"
              disabled={!connected || state !== "running"}
              onClick={s.stop}
            >
              <Square className="size-4" /> Stop
            </Button>
            <Button
              variant="outline"
              className="gap-2"
              disabled={!connected || busy || tooBig}
              onClick={s.saveToBoard}
              title="Run it now and every time the board powers up"
            >
              <Upload className="size-4" /> Save to board
            </Button>
            <Button
              variant="ghost"
              className="gap-2"
              disabled={!connected || !s.status?.saved}
              onClick={s.erase}
              title="Stop running this script at power-up (doesn't stop it now)"
            >
              <Trash2 className="size-4" /> Remove saved
            </Button>
            <span
              className={cn(
                "ml-auto font-mono text-xs tabular-nums text-muted-foreground",
                tooBig && "text-destructive",
              )}
            >
              {s.upload
                ? `uploading ${s.upload.sent}/${s.upload.total} B`
                : `${bytes.toLocaleString()} / ${SCRIPT_MAX_LEN.toLocaleString()} B`}
              {s.status && state === "running" && ` · heap ${(s.status.mem / 1024).toFixed(1)} KB`}
            </span>
          </div>
          {s.error && <p className="text-xs text-destructive">{s.error}</p>}
          {!connected && (
            <p className="text-xs text-muted-foreground">Connect to the Pico to run scripts.</p>
          )}
        </CardContent>
      </Card>

      <div className="grid gap-4 md:grid-cols-2">
        <Card>
          <CardHeader className="pb-3">
            <CardTitle className="flex items-center gap-2 text-sm">
              <Terminal className="size-4" /> Output
            </CardTitle>
            <CardAction>
              <Button variant="ghost" size="sm" className="gap-1.5" onClick={s.clearOutput}>
                <Trash2 className="size-3.5" /> Clear
              </Button>
            </CardAction>
          </CardHeader>
          <CardContent>
            <ScrollArea className="h-64 rounded-md border bg-muted/30 p-3">
              {s.output ? (
                <pre ref={outRef} className="whitespace-pre-wrap break-all font-mono text-xs">
                  {s.output}
                </pre>
              ) : (
                <p className="text-xs text-muted-foreground">
                  print() and OutputLogMessage() show up here. Errors too.
                </p>
              )}
            </ScrollArea>
          </CardContent>
        </Card>

        <Card>
          <CardHeader className="pb-3">
            <CardTitle className="flex items-center gap-2 text-sm">
              <BookOpen className="size-4" /> Functions
            </CardTitle>
          </CardHeader>
          <CardContent>
            <ScrollArea className="h-64 pr-3">
              <dl className="space-y-2.5 text-xs">
                {SCRIPT_API.map((f) => (
                  <div key={f.sig}>
                    <dt className="font-mono font-semibold">{f.sig}</dt>
                    <dd className="text-muted-foreground">{f.doc}</dd>
                  </div>
                ))}
                <div>
                  <dt className="font-semibold">Not available</dt>
                  <dd className="text-muted-foreground">
                    Keyboard output (PressKey…), absolute positioning (MoveMouseTo), macros and
                    M-keys: the board only sits on the mouse. IsModifierPressed / IsKeyLockOn
                    always return false. Standard Lua libraries: string, table, math, coroutine,
                    utf8 (no io / os). 96 KB of memory.
                  </dd>
                </div>
              </dl>
            </ScrollArea>
          </CardContent>
        </Card>
      </div>
    </div>
  );
}
