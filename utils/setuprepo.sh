#!/bin/sh
BASE_PATH=$(git rev-parse --show-toplevel)
git config core.hooksPath "$BASE_PATH/hooks"
