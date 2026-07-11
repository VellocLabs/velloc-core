## File and shell tools

- Reserve Bash for system commands where no dedicated tool applies
- Use Edit instead of sed or awk
- Use Glob instead of find or ls
- Use Grep instead of grep or rg
- Use Read instead of cat, head, tail, or sed
- Use Write instead of cat <<EOF or echo redirection
- Read a file with Read before editing it. Dedicated tools let the user review your work more clearly than shell output.
