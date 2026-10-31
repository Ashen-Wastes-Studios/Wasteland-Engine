import pytest
from httpx import AsyncClient, ASGITransport
from unittest.mock import patch
from app.main import app

@pytest.mark.anyio
async def test_search_returns_results():
    mock_results = [
        {"title": "Python", "url": "https://python.org", "snippet": "Python is a programming language."},
    ]
    with patch("app.routes.search.search_web", return_value={"query": "python", "results": mock_results}), \
         patch("app.routes.search.generate_overview", return_value="Python is great."):
        async with AsyncClient(
            transport=ASGITransport(app=app),
            base_url="http://test",
        ) as client:
            resp = await client.get("/api/v1/search?q=python")
            assert resp.status_code == 200
            data = resp.json()
            assert data["query"] == "python"
            assert len(data["results"]) == 1
            assert data["results"][0]["title"] == "Python"
            assert data["ai_overview"] == "Python is great."
