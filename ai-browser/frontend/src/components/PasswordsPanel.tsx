import type { PasswordEntry } from "../passwords";

export function PasswordsPanel({
  passwords,
  onRemove,
}: {
  passwords: PasswordEntry[];
  onRemove: (id: string) => void;
}) {
  return (
    <div style={{ padding: 12 }}>
      <h3 style={{ margin: "0 0 12px" }}>Passwords</h3>
      {passwords.length === 0 && (
        <p style={{ fontSize: 12, color: "var(--muted-foreground)" }}>No saved passwords.</p>
      )}
      {passwords.map((entry) => (
        <div
          key={entry.id}
          style={{
            padding: "8px 6px",
            borderBottom: "1px solid var(--border)",
            fontSize: 12,
            display: "flex",
            justifyContent: "space-between",
            alignItems: "center",
          }}
        >
          <div>
            <div style={{ fontWeight: 600 }}>{entry.site}</div>
            <div style={{ color: "var(--muted-foreground)" }}>{entry.username}</div>
          </div>
          <button
            onClick={() => onRemove(entry.id)}
            style={{
              padding: "2px 6px",
              background: "transparent",
              border: "1px solid var(--border)",
              color: "var(--foreground)",
              cursor: "pointer",
              borderRadius: 4,
              fontSize: 11,
            }}
          >
            Remove
          </button>
        </div>
      ))}
    </div>
  );
}
