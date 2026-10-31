import type { TabGroup } from "../extensions";

export function TabGroupsPanel({
  groups,
  onSelect,
  onAdd,
}: {
  groups: TabGroup[];
  onSelect: (id: string) => void;
  onAdd: () => void;
}) {
  return (
    <div style={{ padding: 12 }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 8 }}>
        <h3 style={{ margin: 0 }}>Tab Groups</h3>
        <button
          onClick={onAdd}
          style={{
            padding: "2px 8px",
            background: "transparent",
            border: "1px solid var(--border)",
            color: "var(--foreground)",
            cursor: "pointer",
            borderRadius: 4,
            fontSize: 12,
          }}
        >
          +
        </button>
      </div>
      {groups.length === 0 && (
        <p style={{ fontSize: 12, color: "var(--muted-foreground)" }}>No tab groups.</p>
      )}
      {groups.map((group) => (
        <div
          key={group.id}
          onClick={() => onSelect(group.id)}
          style={{
            padding: "6px 8px",
            borderBottom: "1px solid var(--border)",
            fontSize: 12,
            cursor: "pointer",
            borderRadius: 4,
            display: "flex",
            alignItems: "center",
            gap: 8,
          }}
        >
          <span
            style={{
              width: 10,
              height: 10,
              borderRadius: "50%",
              background: group.color,
              display: "inline-block",
            }}
          />
          {group.name}
        </div>
      ))}
    </div>
  );
}
