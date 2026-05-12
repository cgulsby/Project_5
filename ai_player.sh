#!/bin/bash
BROKER="cgulsbycs2600.duckdns.org"
TOPIC_STATUS="tictactoe/status"
TOPIC_MOVE="tictactoe/player2/move"

echo "================================================"
echo "  TicTacToe AI Player (Player 2)"
echo "================================================"
echo "Waiting for game to begin..."

# main loop
while true; do
  STATUS=$(mosquitto_sub -h "$BROKER" -t "$TOPIC_STATUS" -C 1 -W 30)

  if [[ -z "$STATUS" ]]; then
    echo "Timeout waiting for status. Is the game running?"
    exit 1
  fi

  echo "[STATUS] $STATUS"

  # Check for game over
  if [[ "$STATUS" == *"winner"* ]] || [[ "$STATUS" == *"draw"* ]] || [[ "$STATUS" == *"wins"* ]]; then
    echo "Game over! Exiting."
    break
  fi

  # Only act on our turn prompt
  if [[ "$STATUS" != *"Player 2"* ]]; then
    echo "Not our turn, waiting..."
    continue
  fi

  echo "AI's turn! Picking a move..."

  TRIED=()

  # move retry
  while true; do

    # Pick an untried random cell
    while true; do
      CELL=$((RANDOM % 9 + 1))
      if [[ " ${TRIED[*]} " != *" $CELL "* ]]; then
        break
      fi
    done
    TRIED+=("$CELL")

    ROW=$(((CELL - 1) / 3 + 1))
    COL=$(((CELL - 1) % 3 + 1))
    MOVE="$ROW,$COL"

    echo "[AI] Trying cell $CELL -> $MOVE"
    mosquitto_pub -h "$BROKER" -t "$TOPIC_MOVE" -m "$MOVE"

    # Wait for server's response to this move
    RESPONSE=$(mosquitto_sub -h "$BROKER" -t "$TOPIC_STATUS" -C 1 -W 10)

    if [[ -z "$RESPONSE" ]]; then
      echo "Timeout waiting for response. Exiting."
      exit 1
    fi

    echo "[STATUS] $RESPONSE"

    # Game ended on our move
    if [[ "$RESPONSE" == *"winner"* ]] || [[ "$RESPONSE" == *"draw"* ]] || [[ "$RESPONSE" == *"wins"* ]]; then
      echo "Game over detected! Exiting."
      exit 0
    fi

    # Server explicitly rejected the move — try another cell
    if [[ "$RESPONSE" == *"taken"* ]] || [[ "$RESPONSE" == *"Invalid"* ]] || [[ "$RESPONSE" == *"bounds"* ]]; then
      echo "Move rejected, trying another..."
      continue
    fi

    # Server explicitly acknowledged the move — done
    if [[ "$RESPONSE" == *"accepted"* ]]; then
      echo "Move accepted!"
      break
    fi

    # Any other message (e.g. a Player 1 prompt that slipped in) — not our
    # acknowledgment, stay in the retry loop
    echo "Unexpected status ('$RESPONSE'), retrying..."

  done

done

echo "AI Player done."
