# AI Browser

A web browser with an editor layout, AI overviews, and webpage indexing.

## Prerequisites

- Node.js 20+
- Rust + Cargo
- Python 3.12+
- Docker & Docker Compose
- Ollama (for local LLM)

## Quickstart

```bash
# Start services
docker compose up -d

# Backend
cd backend
python -m uvicorn app.main:app --host 127.0.0.1 --port 8000

# Frontend
cd frontend
npm install
npm run tauri dev
```

## Environment

| Variable | Default | Purpose |
|----------|---------|---------|
| `SEARXNG_URL` | `http://localhost:8888` | SearXNG instance |
| `GOOGLE_API_KEY` | - | Google CSE fallback |
| `GOOGLE_CX` | - | Google CSE engine ID |
| `MEILI_URL` | `http://localhost:7700` | Meilisearch |
| `MEILI_KEY` | `masterKey` | Meilisearch key |
| `LLM_URL` | `http://localhost:11434` | Ollama |
| `LLM_MODEL` | `qwen2.5:3b` | Model for overviews |
