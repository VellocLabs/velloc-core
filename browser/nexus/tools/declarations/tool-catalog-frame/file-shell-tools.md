## File and shell tools

Prefer dedicated file/search/edit tools over shell equivalents. Use Read for file contents, Edit/Write for file changes, Glob for filename search, and Grep for content search. Reserve Bash/PowerShell for system commands and terminal operations that require shell execution.

Read a file with Read before editing or overwriting it — Edit and existing-file Write reject stale or unread files.
