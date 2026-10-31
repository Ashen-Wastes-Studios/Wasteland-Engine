import pytest
from unittest.mock import patch
from app.services.indexer import index_document, search_index

@pytest.mark.anyio
async def test_index_and_search():
    mock_index = patch("app.services.indexer._index").start()
    mock_search = patch("app.services.indexer._index.search").start()

    mock_index.add_documents.return_value = None
    mock_search.return_value = {"hits": [{"id": "test-1", "title": "Test Page"}]}

    doc = {"id": "test-1", "title": "Test Page", "url": "https://example.com", "content": "Hello world."}
    await index_document(doc)
    mock_index.add_documents.assert_called_once_with([doc], primary_key="id")

    results = await search_index("hello")
    assert any(r["id"] == "test-1" for r in results)

    patch.stopall()
