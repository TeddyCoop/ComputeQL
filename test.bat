@echo off
cd build

REM call gdb.exe --query="CREATE DATABASE shop; USE shop; CREATE TABLE products (id u32, price f64, name string8); INSERT INTO products (id, price, name) VALUES (1, 9.99, 'widget'), (2, 19.99, 'gadget'), (3, 4.50, 'gizmo');"
call gdb.exe --query="USE shop; SELECT id, name, price FROM products WHERE price > 5.00;"