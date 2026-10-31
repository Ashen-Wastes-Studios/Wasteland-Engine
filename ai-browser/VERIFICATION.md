# AI Browser — Verification Checklist

## Services
- [ ] `docker compose up -d` brings up SearXNG, Meilisearch, Ollama
- [ ] Backend starts on port 8000 and returns `{"status":"ok"}` from `/health`

## Backend
- [ ] `GET /api/v1/search?q=python&ai_overview=true` returns results + `ai_overview` string
- [ ] `POST /api/v1/index/crawl` with `{"url":"https://example.com"}` queues indexing
- [ ] After crawl, Meilisearch returns the page content

## Frontend
- [ ] `npm run tauri dev` opens window with tabs, sidebar, address bar
- [ ] Typing a query in the address bar shows AI overview + results in split pane
- [ ] Typing a URL loads the page in the webview
- [ ] Command palette opens on Ctrl+Shift+P

## Tests
- [ ] `cd ai-browser/backend && python -m pytest -v` passes
