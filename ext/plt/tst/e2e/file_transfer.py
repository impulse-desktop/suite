from session import Session

with Session("file_transfer") as s:
    first = s.artifacts / "first.bin"
    second = s.artifacts / "second.bin"
    output = s.artifacts / "received.bin"
    first.write_bytes(bytes(range(256)) * 4096)
    second.write_bytes(bytes(range(255, -1, -1)) * 3072)
    s.launch(PLT_TRANSFER_FIRST=str(first), PLT_TRANSFER_SECOND=str(second), PLT_TRANSFER_OUTPUT=str(output))
    s.logged("IDLE TIMEOUT")
    s.logged("WAITING FOR TRANSACTION")
    s.logged(f"TRANSFERRED {first.stat().st_size + second.stat().st_size}")
    assert output.read_bytes() == first.read_bytes() + second.read_bytes()
    s.screenshot("completed", [(.1, .5, .9, .8, "40a060")])
    s.close()
