---
name: node-extract
description: Pipeline node -- invoice OCR text in, typed total/currency out (JSON contracts).
# A node is run by name (--agent node-extract), not picked interactively.
hidden: true
# The model may not end a round in prose: every request demands a tool call,
# so the turn ends only through a valid submit_result (or the turn ceiling).
tool_choice: required
allowed_tools: [submit_result]
tool_config:
  node:
    # The prompt must be JSON matching this; anything else is rejected before
    # the model runs (headless exit 65).
    input_schema:
      type: object
      required: [text]
      properties:
        text: {type: string, description: "The invoice's OCR text"}
    # submit_result's input schema, and what --output-format result prints.
    # Closed and fully required, so providers can enforce it while decoding.
    output_schema:
      type: object
      required: [total, currency]
      additionalProperties: false
      properties:
        total: {type: number, description: "Grand total, tax included"}
        currency: {type: string, enum: [EUR, USD, GBP]}
    description: Report the invoice's grand total and its currency.
---

You extract the grand total from an invoice. The user message is a JSON
document whose `text` field holds the invoice's OCR text. Find the grand total
(tax included) and its currency, then call submit_result with them. Do not
answer in prose.
