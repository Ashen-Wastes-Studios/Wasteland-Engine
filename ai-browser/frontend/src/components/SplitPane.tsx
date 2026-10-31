import { useState, useCallback } from "react";

type Direction = "horizontal" | "vertical";

export function SplitPane({
  direction = "vertical",
  left,
  right,
  defaultLeft = 300,
}: {
  direction?: Direction;
  left: React.ReactNode;
  right: React.ReactNode;
  defaultLeft?: number;
}) {
  const [size, setSize] = useState(defaultLeft);
  const [dragging, setDragging] = useState(false);

  const onMouseDown = useCallback(() => setDragging(true), []);
  const onMouseMove = useCallback(
    (e: React.MouseEvent) => {
      if (!dragging) return;
      setSize(direction === "vertical" ? e.clientY : e.clientX);
    },
    [dragging, direction],
  );
  const onMouseUp = useCallback(() => setDragging(false), []);

  return (
    <div
      style={{
        display: "flex",
        flexDirection: direction === "vertical" ? "column" : "row",
        height: "100%",
        width: "100%",
      }}
      onMouseMove={onMouseMove}
      onMouseUp={onMouseUp}
      onMouseLeave={onMouseUp}
    >
      <div
        style={{
          width: direction === "vertical" ? "100%" : size,
          height: direction === "vertical" ? size : "100%",
          overflow: "auto",
        }}
      >
        {left}
      </div>
      <div
        onMouseDown={onMouseDown}
        style={{
          cursor: direction === "vertical" ? "row-resize" : "col-resize",
          background: dragging ? "var(--accent)" : "var(--border)",
          width: direction === "vertical" ? "100%" : 4,
          height: direction === "vertical" ? 4 : "100%",
          flexShrink: 0,
        }}
      />
      <div style={{ flex: 1, minWidth: 0, minHeight: 0, overflow: "auto" }}>{right}</div>
    </div>
  );
}
