# nanopdf MCP Examples

This directory contains examples and tests for the nanopdf MCP (Model Context Protocol) server.

## Files

- `test-mcp-json.cc` - Unit tests for the JSON parser library
- `test-mcp-server.sh` - Integration test script for the MCP server (adjust paths as needed)

## Building

The MCP server is built automatically when building nanopdf:

```bash
cd ../../..
mkdir build && cd build
cmake ..
make nanopdf-mcp
```

The executable will be at: `build/nanopdf-mcp`

## Testing

Run the JSON library tests:

```bash
cd build
./test_mcp_json
```

## Usage

See the main [MCP_USAGE.md](../../MCP_USAGE.md) documentation for complete usage instructions.

## Quick Test

Test the server manually:

```bash
# Initialize the server
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' | ./build/nanopdf-mcp

# Should return initialization response with server info
```

## Integration with Claude Desktop

Add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "nanopdf": {
      "command": "/absolute/path/to/build/nanopdf-mcp"
    }
  }
}
```

Then restart Claude Desktop.
