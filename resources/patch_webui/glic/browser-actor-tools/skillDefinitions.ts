import type {HostedToolDefinition} from './types.js'

export const BROWSER_SKILL_PREVIEWS_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_skill_previews',
  searchHint: 'browser hosted skill previews',
  readOnly: true,
  description:
    'List the browser-hosted GLIC skills currently available for invocation.',
  parameters: {
    type: 'object',
    properties: {},
  },
}

export const BROWSER_GET_SKILL_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_get_skill',
  searchHint: 'browser hosted skill details',
  readOnly: true,
  description:
    'Fetch the full prompt and metadata for a browser-hosted GLIC skill by id.',
  parameters: {
    type: 'object',
    properties: {
      id: {
        type: 'string',
        description: 'Skill id from browser_skill_previews.',
      },
    },
    required: ['id'],
  },
}

export const BROWSER_CREATE_SKILL_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_create_skill',
  searchHint: 'browser hosted create skill',
  description:
    'Open the browser-hosted GLIC create-skill flow with the provided prompt and optional metadata.',
  parameters: {
    type: 'object',
    properties: {
      prompt: {
        type: 'string',
        description: 'Prompt text for the new skill.',
      },
      id: {
        type: 'string',
        description: 'Optional stable skill id for editing/remix flows.',
      },
      name: {
        type: 'string',
        description: 'Optional display name for the skill.',
      },
      icon: {
        type: 'string',
        description: 'Optional icon identifier.',
      },
      description: {
        type: 'string',
        description: 'Optional short skill description.',
      },
    },
    required: ['prompt'],
  },
}

export const BROWSER_MANAGE_SKILLS_TOOL: HostedToolDefinition = {
  type: 'function',
  name: 'browser_manage_skills',
  searchHint: 'browser hosted manage skills',
  description:
    'Open the browser-hosted GLIC skills management UI.',
  parameters: {
    type: 'object',
    properties: {},
  },
}

export const BROWSER_SKILL_TOOL_DEFINITIONS: readonly HostedToolDefinition[] = [
  BROWSER_SKILL_PREVIEWS_TOOL,
  BROWSER_GET_SKILL_TOOL,
  BROWSER_CREATE_SKILL_TOOL,
  BROWSER_MANAGE_SKILLS_TOOL,
]

export function isBrowserSkillTool(toolName: string): boolean {
  return BROWSER_SKILL_TOOL_DEFINITIONS.some(tool => tool.name === toolName)
}
