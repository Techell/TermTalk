"""Validate the project roadmap and render a conversation visualization snapshot."""

import argparse
import json
from pathlib import Path


def validate(data):
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported schema_version")
    stages = [stage["id"] for stage in data["stages"]]
    if len(stages) != len(set(stages)):
        raise ValueError("Duplicate stage ID")
    tasks = data["tasks"]
    by_id = {task["id"]: task for task in tasks}
    if len(by_id) != len(tasks):
        raise ValueError("Duplicate task ID")
    states = {"planned", "in_progress", "review", "blocked", "done"}
    for task in tasks:
        if task["stage"] not in stages or task["status"] not in states:
            raise ValueError(f"Invalid stage/status: {task['id']}")
        if not task.get("criteria") or not task.get("title"):
            raise ValueError(f"Missing acceptance criteria/title: {task['id']}")
        if task["status"] == "done" and not task.get("evidence"):
            raise ValueError(f"Done task has no evidence: {task['id']}")
        if task["status"] == "blocked" and not task.get("blocker"):
            raise ValueError(f"Blocked task has no reason: {task['id']}")
        for dep in task["deps"]:
            if dep not in by_id:
                raise ValueError(f"Unknown dependency {dep}: {task['id']}")
            if task["status"] == "done" and by_id[dep]["status"] != "done":
                raise ValueError(f"Done task has unfinished dependency: {task['id']}")
    visiting, visited = set(), set()

    def visit(task_id):
        if task_id in visiting:
            raise ValueError(f"Dependency cycle at {task_id}")
        if task_id in visited:
            return
        visiting.add(task_id)
        for dep in by_id[task_id]["deps"]:
            visit(dep)
        visiting.remove(task_id)
        visited.add(task_id)

    for task_id in by_id:
        visit(task_id)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--source", type=Path, default=root / "docs/roadmap.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.source.read_text(encoding="utf-8"))
    validate(data)
    # Escape HTML-sensitive characters so data cannot terminate the script element.
    payload = json.dumps(data, ensure_ascii=False).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    template = Path(__file__).with_name("roadmap.template.html").read_text(encoding="utf-8")
    if template.count("__ROADMAP_DATA__") != 1:
        raise ValueError("Expected one template data marker")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(template.replace("__ROADMAP_DATA__", payload), encoding="utf-8")
    print(f"Validated {len(data['tasks'])} tasks; visualization: {args.output}")


if __name__ == "__main__":
    main()
