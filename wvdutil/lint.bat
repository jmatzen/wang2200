echo off

echo === running pylint ===
py -3 -m pylint wvdutil.py wvdlib.py WvdHandler_base.py WvdHandler_basic.py WvdHandler_data.py WvdHandler_edit.py

echo === running mypy ===
mypy wvdutil.py
