#pragma once
namespace uav::gateway::detail {
inline constexpr const char *openapi = R"UAV_OPENAPI({
  "openapi": "3.0.3",
  "info": {
    "title": "UAV Gateway API",
    "version": "1.0.0",
    "description": "Web/API control plane. GB28181 and ONVIF have independent native entries. JSON envelopes; X-Request-ID response header. Provisioned tokens remain usable by API clients; login sessions are revocable. TLS and static asset hosting are configurable."
  },
  "servers": [
    {
      "url": "/"
    }
  ],
  "security": [
    {
      "bearerAuth": []
    }
  ],
  "paths": {
    "/healthz": {
      "get": {
        "summary": "Gateway liveness",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "security": []
      }
    },
    "/readyz": {
      "get": {
        "summary": "Gateway readiness",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "security": []
      }
    },
    "/api/v1/auth/login": {
      "post": {
        "summary": "Exchange a provisioned token for a session",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "security": [],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Login"
              }
            }
          }
        }
      }
    },
    "/api/v1/auth/refresh": {
      "post": {
        "summary": "Rotate access and refresh credentials",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "security": [],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Refresh"
              }
            }
          }
        }
      }
    },
    "/api/v1/auth/me": {
      "get": {
        "summary": "Current principal",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/auth/logout": {
      "post": {
        "summary": "Revoke the current login session",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/status": {
      "get": {
        "summary": "Device and gateway status",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/devices": {
      "get": {
        "summary": "List devices",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/devices/{id}": {
      "parameters": [
        {
          "name": "id",
          "in": "path",
          "required": true,
          "schema": {
            "type": "string",
            "pattern": "^[A-Za-z0-9_.-]{1,64}$"
          }
        }
      ],
      "get": {
        "summary": "Get device",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      },
      "put": {
        "summary": "Create or replace an unattached device",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Device"
              }
            }
          }
        }
      },
      "delete": {
        "summary": "Delete an unattached device",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/streams": {
      "get": {
        "summary": "List streams",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/streams/{id}": {
      "parameters": [
        {
          "name": "id",
          "in": "path",
          "required": true,
          "schema": {
            "type": "string",
            "pattern": "^[A-Za-z0-9_.-]{1,64}$"
          }
        }
      ],
      "get": {
        "summary": "Get stream",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      },
      "put": {
        "summary": "Create or replace an unattached stream",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Stream"
              }
            }
          }
        }
      },
      "delete": {
        "summary": "Delete an unattached stream",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/devices/{id}/commands": {
      "parameters": [
        {
          "name": "id",
          "in": "path",
          "required": true,
          "schema": {
            "type": "string",
            "pattern": "^[A-Za-z0-9_.-]{1,64}$"
          }
        }
      ],
      "post": {
        "summary": "Submit a device command",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Command"
              }
            }
          }
        }
      }
    },
    "/api/v1/commands": {
      "get": {
        "summary": "Most recent 32 command records",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/commands/{id}": {
      "parameters": [
        {
          "name": "id",
          "in": "path",
          "required": true,
          "schema": {
            "type": "string",
            "pattern": "^[A-Za-z0-9_.-]{1,64}$"
          }
        }
      ],
      "get": {
        "summary": "Get command result",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      },
      "delete": {
        "summary": "Cancel a queued command",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/playback": {
      "post": {
        "summary": "Resolve endpoint, authenticate, and issue a scoped media grant",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Playback"
              }
            }
          }
        }
      }
    },
    "/api/v1/playback/{id}": {
      "parameters": [
        {
          "name": "id",
          "in": "path",
          "required": true,
          "schema": {
            "type": "string",
            "pattern": "^[A-Za-z0-9_.-]{1,64}$"
          }
        }
      ],
      "get": {
        "summary": "Inspect a playback grant",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      },
      "delete": {
        "summary": "Revoke a playback grant",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/playback/{id}/renew": {
      "parameters": [
        {
          "name": "id",
          "in": "path",
          "required": true,
          "schema": {
            "type": "string",
            "pattern": "^[A-Za-z0-9_.-]{1,64}$"
          }
        }
      ],
      "post": {
        "summary": "Rotate media token and renew grant",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/ws-ticket": {
      "post": {
        "summary": "Issue a single-use WebSocket ticket",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/events": {
      "get": {
        "summary": "WebSocket upgrade with single-use ticket",
        "responses": {
          "101": {
            "description": "WebSocket opened; JSON events snapshot, command.accepted, command.result, registry.changed and backend topics"
          },
          "401": {
            "description": "Invalid or consumed ticket"
          }
        },
        "security": [],
        "parameters": [
          {
            "name": "ticket",
            "in": "query",
            "required": true,
            "schema": {
              "type": "string"
            }
          }
        ]
      }
    },
    "/api/v1/media/authorize": {
      "post": {
        "summary": "Media service introspection using its own service credential",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        },
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/MediaAuthorization"
              }
            }
          }
        },
        "security": [
          {
            "mediaServiceAuth": []
          }
        ]
      }
    },
    "/api/v1/audit": {
      "get": {
        "summary": "Latest 256 audit records (admin)",
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "202": {
            "$ref": "#/components/responses/Status202"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "401": {
            "$ref": "#/components/responses/Status401"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "429": {
            "$ref": "#/components/responses/Status429"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/webrtc": {
      "get": {
        "summary": "Static console alias; playback requires credentials",
        "security": [],
        "responses": {
          "200": {
            "description": "Built frontend index.html",
            "content": {
              "text/html": {
                "schema": {
                  "type": "string"
                }
              }
            }
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    },
    "/api/v1/webrtc/{stream_id}": {
      "post": {
        "summary": "Create live video session from a complete SDP offer",
        "description": "Bearer credential must be a media_token issued for this stream and protocol webrtc. Wait for browser ICE gathering to complete. Requires an attached media adapter; default one video subscriber. Response uses the standard envelope with data {type:answer,sdp,session_id}. Session ID differs from the playback grant ID. No PATCH/trickle or same-session renegotiation.",
        "parameters": [
          {
            "name": "stream_id",
            "in": "path",
            "required": true,
            "schema": {
              "type": "string",
              "pattern": "^[A-Za-z0-9_.-]{1,64}$"
            }
          }
        ],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "required": [
                  "sdp"
                ],
                "properties": {
                  "sdp": {
                    "type": "string",
                    "maxLength": 61440
                  }
                }
              }
            }
          }
        },
        "responses": {
          "201": {
            "$ref": "#/components/responses/Status201"
          },
          "400": {
            "$ref": "#/components/responses/Status400"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "409": {
            "$ref": "#/components/responses/Status409"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          },
          "406": {
            "description": "Receiving H264 Baseline packetization-mode=1 video required"
          }
        }
      }
    },
    "/api/v1/webrtc/{stream_id}/{session_id}": {
      "delete": {
        "summary": "Close a live video session and release its subscriber slot",
        "description": "Uses the current media_token for the playback grant that created this session. Token rotation during renewal preserves the existing session. Revocation or expiry closes the session.",
        "parameters": [
          {
            "name": "stream_id",
            "in": "path",
            "required": true,
            "schema": {
              "type": "string",
              "pattern": "^[A-Za-z0-9_.-]{1,64}$"
            }
          },
          {
            "name": "session_id",
            "in": "path",
            "required": true,
            "schema": {
              "type": "string",
              "pattern": "^[A-Za-z0-9_.-]{1,64}$"
            }
          }
        ],
        "responses": {
          "200": {
            "$ref": "#/components/responses/Status200"
          },
          "403": {
            "$ref": "#/components/responses/Status403"
          },
          "404": {
            "$ref": "#/components/responses/Status404"
          },
          "503": {
            "$ref": "#/components/responses/Status503"
          }
        }
      }
    }
  },
  "components": {
    "securitySchemes": {
      "bearerAuth": {
        "type": "http",
        "scheme": "bearer"
      },
      "mediaServiceAuth": {
        "type": "http",
        "scheme": "bearer",
        "description": "UAV_GATEWAY_MEDIA_TOKEN; separate from user credentials"
      }
    },
    "schemas": {
      "Login": {
        "type": "object",
        "required": [
          "token"
        ],
        "properties": {
          "token": {
            "type": "string",
            "maxLength": 4096
          }
        }
      },
      "Refresh": {
        "type": "object",
        "required": [
          "refresh_token"
        ],
        "properties": {
          "refresh_token": {
            "type": "string"
          }
        }
      },
      "Device": {
        "type": "object",
        "required": [
          "type"
        ],
        "properties": {
          "type": {
            "enum": [
              "camera",
              "radar",
              "spectrum",
              "ptz",
              "laser"
            ]
          },
          "name": {
            "type": "string",
            "maxLength": 128
          },
          "enabled": {
            "type": "boolean"
          },
          "config": {
            "type": "object"
          }
        }
      },
      "Stream": {
        "type": "object",
        "required": [
          "device_id",
          "protocols"
        ],
        "properties": {
          "device_id": {
            "type": "string"
          },
          "codec": {
            "enum": [
              "h264",
              "h265"
            ]
          },
          "protocols": {
            "type": "array",
            "minItems": 1,
            "maxItems": 4,
            "uniqueItems": true,
            "items": {
              "enum": [
                "rtsp",
                "webrtc",
                "moq",
                "gb28181"
              ]
            }
          }
        }
      },
      "Command": {
        "type": "object",
        "required": [
          "action"
        ],
        "properties": {
          "action": {
            "type": "string",
            "maxLength": 64
          },
          "parameters": {
            "type": "object"
          },
          "ttl_ms": {
            "type": "integer",
            "minimum": 100,
            "maximum": 10000,
            "default": 2000
          }
        }
      },
      "Playback": {
        "type": "object",
        "required": [
          "stream_id",
          "protocol"
        ],
        "properties": {
          "stream_id": {
            "type": "string"
          },
          "protocol": {
            "enum": [
              "rtsp",
              "webrtc",
              "moq"
            ]
          }
        }
      },
      "MediaAuthorization": {
        "type": "object",
        "required": [
          "token",
          "stream_id",
          "protocol"
        ],
        "properties": {
          "token": {
            "type": "string"
          },
          "stream_id": {
            "type": "string"
          },
          "protocol": {
            "type": "string"
          }
        }
      },
      "Envelope": {
        "type": "object",
        "required": [
          "ok"
        ],
        "properties": {
          "ok": {
            "type": "boolean"
          },
          "data": {},
          "error": {
            "type": "object",
            "properties": {
              "code": {
                "type": "string"
              },
              "message": {
                "type": "string"
              }
            }
          }
        }
      }
    },
    "responses": {
      "Status200": {
        "description": "Success",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status201": {
        "description": "Created",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status202": {
        "description": "Accepted",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status400": {
        "description": "Invalid request",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status401": {
        "description": "Unauthorized",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status403": {
        "description": "Forbidden",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status404": {
        "description": "Not found",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status409": {
        "description": "Conflict",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status429": {
        "description": "Resource limit",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      },
      "Status503": {
        "description": "Backend or persistence unavailable",
        "content": {
          "application/json": {
            "schema": {
              "$ref": "#/components/schemas/Envelope"
            }
          }
        }
      }
    }
  }
}
)UAV_OPENAPI";
}
