#!/bin/bash
#
# Simple integration test for nanopdf MCP server
#

set -e

SERVER="../../../build/nanopdf-mcp"
TEST_PDF="../../../tests/fixtures/visual/blank.pdf"

if [ ! -f "$SERVER" ]; then
    echo "Error: MCP server not found at $SERVER"
    echo "Please build it first: make nanopdf-mcp"
    exit 1
fi

if [ ! -f "$TEST_PDF" ]; then
    echo "Error: Test PDF not found at $TEST_PDF"
    exit 1
fi

echo "Testing nanopdf MCP server..."
echo

# Test 1: Initialize
echo "Test 1: Initialize handshake"
INIT_REQUEST='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'
INIT_RESPONSE=$(echo "$INIT_REQUEST" | timeout 2 "$SERVER" | head -1)

if echo "$INIT_RESPONSE" | grep -q '"protocolVersion"'; then
    echo "✓ Initialize succeeded"
else
    echo "✗ Initialize failed"
    echo "Response: $INIT_RESPONSE"
    exit 1
fi

# Test 2: List tools
echo
echo "Test 2: List tools"
LIST_REQUEST='{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

# Create a simple script to send both initialize and tools/list
{
    echo "$INIT_REQUEST"
    echo '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'
    echo "$LIST_REQUEST"
} | timeout 2 "$SERVER" > /tmp/mcp_test_output.txt

TOOLS_RESPONSE=$(tail -1 /tmp/mcp_test_output.txt)

if echo "$TOOLS_RESPONSE" | grep -q 'load_pdf'; then
    echo "✓ Tools list succeeded"
    TOOL_COUNT=$(echo "$TOOLS_RESPONSE" | grep -o 'load_pdf\|get_page_count\|extract_text\|get_page_info\|get_metadata\|extract_text_layout\|find_text\|get_fonts\|get_images\|close_pdf' | wc -l)
    echo "  Found $TOOL_COUNT tools"
else
    echo "✗ Tools list failed"
    echo "Response: $TOOLS_RESPONSE"
    exit 1
fi

echo
echo "All tests passed! ✓"
echo
echo "The MCP server is working correctly."
echo "You can now configure it in Claude Desktop using:"
echo "  Path: $(realpath $SERVER)"
