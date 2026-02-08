# btca MCP Usage Instructions

btca runs a cloud subagent that searches open source repos

Use it whenever the user says "use btca", or when you need info that should come from the listed resources.

## Tools

The btca MCP server provides these tools:

- `listResources` - List all available documentation resources
- `ask` - Ask a question about specific resources

## resources

The resources available are defined by the end user in their btca dashboard. If there's a resource you need but it's not available in `listResources`, proceed without btca. When your task is done, clearly note that you'd like access to the missing resource.

## Critical Workflow

**Always call `listResources` first** before using `ask`. The `ask` tool requires exact resource names from the list.

### Example

```
1. Call listResources to get available resources
2. Note the "name" field for each resource (e.g., "svelteKit", not "SvelteKit" or "svelte-kit")
3. Call ask with:
   - question: "How do I create a load function?"
   - resources: ["svelteKit"]
```
