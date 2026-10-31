import { useState, useEffect, useRef } from "react";

export function CommandPalette({
  open,
  onClose,
  onSelect,
}: {
  open: boolean;
  onClose: () => void;
  onSelect: (cmd: string) => void;
}) {
  const [query, setQuery] = useState("");
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (open) inputRef.current?.focus();
  }, [open]);

  if (!open) return null;

  const commands = [
    { label: "New Tab", action: "tab.new" },
    { label: "Search...", action: "search.focus" },
    { label: "Go to URL", action: "url.focus" },
    { label: "Toggle Sidebar", action: "sidebar.toggle" },
    { label: "Toggle Settings", action: "settings.open" },
    { label: "Bookmark Current Page", action: "bookmark.add" },
    { label: "Toggle AI Overview", action: "ai.toggle" },
  ];

  const filtered = commands.filter((c) =>
    c.label.toLowerCase().includes(query.toLowerCase())
  );

  return (
    <div
      style={{
        position: "fixed",
        inset: 0,
        background: "rgba(0,0,0,0.5)",
        display: "flex",
        justifyContent: "center",
        paddingTop: "15vh",
        zIndex: 1000,
      }}
      onClick={onClose}
    >
      <div
        style={{
          background: "var(--card)",
          border: "1px solid var(--border)",
          borderRadius: 8,
          width: 400,
          maxHeight: 400,
          display: "flex",
          flexDirection: "column",
        }}
        onClick={(e) => e.stopPropagation()}
      >
        <input
          ref={inputRef}
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Type a command..."
          style={{
            width: "100%",
            padding: 12,
            border: "none",
            background: "transparent",
            color: "var(--foreground)",
            outline: "none",
          }}
        />
        <div style={{ overflowY: "auto" }}>
          {filtered.map((cmd) => (
            <div
              key={cmd.action}
              onClick={() => {
                onSelect(cmd.action);
                onClose();
              }}
              style={{
                padding: "8px 12px",
                cursor: "pointer",
                borderBottom: "1px solid var(--border)",
              }}
            >
              {cmd.label}
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
