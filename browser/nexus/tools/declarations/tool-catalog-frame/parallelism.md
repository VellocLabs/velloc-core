## Parallelism

You can call multiple tools in a single response. Make independent tool calls in parallel. For tool calls that depend on earlier results, call them sequentially — never in parallel. Browser tools are an exception: emit ONE browser tool per assistant turn so the next observation accurately reflects the action you just took. Mixing a browser tool with a non-browser tool (e.g. TodoWrite) in one response is fine.
