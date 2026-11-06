# Tools

Steps of the build (run by `SConstruct`): `build_ffmpeg.sh`, `extract_server_dbc.py`, `make_glue_toc.py`, `make_ps_shaders.py`,
`make_remote_manifest.py`, `mpq.py`, `shell_art.py`, `game_install.py`. Generators of committed files:
`make_sandbox_data.py` (`WoW-sandbox-data.inc`), `audio-test/make_sample.py` (`sample-mp3.inc`).

Data: `make_data_manifest.py` (`manifest.txt` of a client directory).

The proxy the page uses to reach a real server: `proxy/wow-proxy.js`.
