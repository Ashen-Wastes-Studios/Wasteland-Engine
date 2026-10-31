import { useState } from "react";
import { useIndexSearch } from "../hooks/useIndexSearch";

export function IndexPanel() {
  const [query, setQuery] = useState("");
  const { search, loading, results, error } = useIndexSearch();

  const handleSearch = () => {
    const trimmed = query.trim();
    if (!trimmed) return;
    search(trimmed);
  };

  return (
    <div>
      <input
        value={query}
        onChange={(e) => setQuery(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Enter") handleSearch();
        }}
        placeholder="Search indexed pages..."
        style={{
          width: "100%",
          padding: "4px 6px",
          marginBottom: 8,
          background: "var(--border)",
          color: "var(--foreground)",
          border: "1px solid var(--border)",
          borderRadius: 4,
          outline: "none",
          fontSize: 12,
        }}
      />
      {error && (
        <div style={{ fontSize: 11, color: "#ffb3b3", marginBottom: 4 }}>
          {error}
        </div>
      )}
      {loading && (
        <div style={{ fontSize: 11, color: "var(--muted-foreground)", marginBottom: 4 }}>
          Searching...
        </div>
      )}
      {results.length === 0 && !loading && !error && (
        <div style={{ fontSize: 11, color: "var(--muted-foreground)" }}>No indexed pages yet.</div>
      )}
      {results.map((r, i) => (
        <div
          key={r.id || i}
          onClick={() => window.open(r.url, "_blank")}
          style={{
            fontSize: 11,
            padding: "4px 6px",
            cursor: "pointer",
            borderRadius: 4,
            whiteSpace: "nowrap",
            overflow: "hidden",
            textOverflow: "ellipsis",
            marginBottom: 4,
          }}
          title={r.url}
        >
          <div style={{ fontWeight: 600 }}>{r.title || r.url}</div>
          {r.snippet && (
            <div style={{ color: "var(--muted-foreground)", fontSize: 10 }}>
              {r.snippet}
            </div>
          )}
        </div>
      ))}
    </div>
  );
}
