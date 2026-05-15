// Template for secrets.h. Copy this file to secrets.h and fill in your values.
// secrets.h is .gitignored; secrets.example.h is committed as documentation.
#pragma once

// Bore.pub tunnel IP for your Pi. Get with: dig +short bore.pub
static const char TRACCAR_HOST[] = "BORE_PUB_IP";

// Shared secret with the Pi-side HMAC proxy. Must match hmac-proxy's
// HMAC_SECRET env var. Generate with: openssl rand -hex 32
static const char HMAC_SECRET[]  = "CHANGE_ME_USE_A_LONG_RANDOM_STRING";
