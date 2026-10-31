import pytest
from unittest.mock import patch, MagicMock
from app.services.ai import generate_overview

@pytest.mark.anyio
async def test_generate_overview_returns_string():
    mock_resp = MagicMock()
    mock_resp.raise_for_status.return_value = None
    mock_resp.json.return_value = {
        "message": {"content": "Python is a popular programming language [Python]."}
    }

    with patch("app.services.ai.httpx.AsyncClient") as MockClient:
        MockClient.return_value.__aenter__.return_value.post.return_value = mock_resp

        results = [
            {"title": "Python", "url": "https://python.org", "snippet": "Python is a programming language."},
        ]
        overview = await generate_overview("What is Python?", results)
        assert isinstance(overview, str)
        assert len(overview) > 0
        assert "Python" in overview
