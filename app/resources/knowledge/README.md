# Assistant knowledge base

Short, curated articles the in-app assistant retrieves from to answer "how do
I..." and "what does this mean" questions (architecture-v3 section 4).

These are deliberately **not** copies of README.md or STATUS.md. Those are
written for contributors and run to tens of kilobytes; retrieval over them
returns build logs and verification tables to a person asking how to unlock a
node. Each article here answers one user-facing question in a few paragraphs,
with a `# Title` and `## Section` headings the retriever splits on.

Everything in this directory is bundled into the installer and read locally.
Nothing is fetched.
