import type { Bookmark } from "../shared-types";

export function NewTabPage({ bookmarks, onNavigate }: { bookmarks: Bookmark[]; onNavigate: (url: string) => void }) {
  return (
    <div
      style={{
        height: "100%",
        display: "flex",
        flexDirection: "column",
        alignItems: "center",
        justifyContent: "center",
        gap: 24,
        padding: 24,
      }}
    >
      <h1 style={{ fontSize: 48, fontWeight: 300, color: "var(--accent)" }}>AI Browser</h1>
      <p style={{ color: "var(--muted-foreground)", fontSize: 14 }}>Search or enter a URL to begin.</p>
      {bookmarks.length > 0 && (
        <div style={{ display: "flex", gap: 12, flexWrap: "wrap", justifyContent: "center" }}>
          {bookmarks.slice(0, 12).map((bookmark, i) => (
            <button
              key={`${bookmark.url}-${i}`}
              onClick={() => onNavigate(bookmark.url)}
              style={{
                padding: "8px 16px",
                background: "var(--border)",
                border: "1px solid var(--border)",
                color: "var(--foreground)",
                cursor: "pointer",
                borderRadius: 20,
                fontSize: 13,
              }}
            >
              {bookmark.title || bookmark.url}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
