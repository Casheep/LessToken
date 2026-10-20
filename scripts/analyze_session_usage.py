"""Read explicit Codex JSONL files locally; emit usage counts, never chat text.

Modern token_usage_record.usage is authoritative when present. event_msg
token_count is a fallback only; its total_token_usage must never be summed.
Reasoning is a subset of output; cached input is a subset of input.
"""
import argparse
import json
from pathlib import Path

FIELDS = ("input_tokens", "cached_input_tokens", "cache_write_input_tokens",
          "output_tokens", "reasoning_output_tokens", "total_tokens")


def validate_usage(value):
    if not isinstance(value, dict):
        raise ValueError("usage must be an object")
    required = ("input_tokens", "output_tokens")
    if any(key not in value for key in required):
        raise ValueError("usage is missing input/output counters")
    out = {}
    for field in FIELDS:
        number = value.get(field, 0)
        if isinstance(number, bool) or not isinstance(number, int) or number < 0:
            raise ValueError("usage counters must be nonnegative integers")
        out[field] = number
    if out["cached_input_tokens"] > out["input_tokens"]:
        raise ValueError("cached input exceeds input")
    if out["reasoning_output_tokens"] > out["output_tokens"]:
        raise ValueError("reasoning output exceeds output")
    expected = out["input_tokens"] + out["output_tokens"]
    if "total_tokens" in value and out["total_tokens"] != expected:
        raise ValueError("total tokens do not match input plus output")
    out["total_tokens"] = expected
    return out


def analyze(records, turn_id=None):
    native, fallback, contexts = [], [], {}
    current_turn = None
    for ordinal, record in enumerate(records):
        kind, value = record.get("type"), record.get("payload", {})
        if not isinstance(value, dict):
            continue
        if kind == "turn_context":
            current_turn = value.get("turn_id")
            contexts[current_turn] = {"model": value.get("model"), "effort": value.get("effort")}
        elif kind == "token_usage_record":
            identity = value.get("response_id") or ("row", ordinal)
            native.append((identity, value.get("turn_id", current_turn), value.get("usage")))
        elif kind == "event_msg" and value.get("type") == "token_count":
            info = value.get("info")
            if not isinstance(info, dict) or not info.get("last_token_usage"):
                continue
            # Notifications can repeat an unchanged cumulative counter.
            total = info.get("total_token_usage")
            identity = json.dumps(total, sort_keys=True) if total else ("row", ordinal)
            fallback.append((identity, current_turn, info["last_token_usage"]))
    selected = native if native else fallback
    by_turn, seen = {}, set()
    for identity, turn, value in selected:
        if identity in seen:
            continue
        seen.add(identity)
        if turn_id is not None and turn != turn_id:
            continue
        usage = validate_usage(value)
        group = by_turn.setdefault(turn, {"model": contexts.get(turn, {}).get("model"),
                                         "effort": contexts.get(turn, {}).get("effort"),
                                         "model_calls": 0, "usage": {key: 0 for key in FIELDS}})
        group["model_calls"] += 1
        for key in FIELDS:
            group["usage"][key] += usage[key]
    totals = {key: sum(group["usage"][key] for group in by_turn.values()) for key in FIELDS}
    totals["uncached_input_tokens"] = totals["input_tokens"] - totals["cached_input_tokens"]
    totals["visible_output_tokens"] = totals["output_tokens"] - totals["reasoning_output_tokens"]
    return {"source": "token_usage_record.usage" if native else "token_count.last_token_usage",
            "model_calls": sum(group["model_calls"] for group in by_turn.values()),
            "totals": totals, "turns": list(by_turn.values()),
            "measurement": "Recorded model usage, not a billing or subscription-quota measurement. "
                           "Cached input is included in input; reasoning is included in output. "
                           "No content, filesystem path, response ID or turn ID is exported. "
                           "Savings require matched baseline/candidate tasks and quality checks."}


def read_records(path):
    # A running session may end in a partially written record. Ignore only that
    # final fragment; malformed complete rows are errors, never silent omissions.
    with Path(path).open(encoding="utf-8") as stream:
        for row in stream:
            try:
                value = json.loads(row)
            except ValueError:
                if not row.endswith("\n"):
                    return
                raise ValueError("Malformed complete JSONL record") from None
            if not isinstance(value, dict):
                raise ValueError("JSONL record must be an object")
            yield value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", help="Explicit local session JSONL path; never scans other chats")
    parser.add_argument("--turn-id", help="Optional exact turn filter; excluded from output")
    parser.add_argument("--output", help="Optional aggregate JSON output")
    args = parser.parse_args()
    try:
        result = analyze(read_records(args.session), args.turn_id)
        if not result["model_calls"]:
            parser.error("No matching model usage records")
    except (OSError, ValueError) as error:
        # Do not print arbitrary source lines or exception text containing paths.
        parser.error(type(error).__name__ + ": could not validate local usage records")
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
