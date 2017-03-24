#! /usr/bin/env python
# -*- coding: utf-8 -*-

__copyright__ = "(c) 2009 RenaissanceRe IP Holdings Ltd.  All rights reserved."

import bcp

ROW_DATA = [
    ['me', 2],
    ['you', 1],
    ['them', 12.177],
    ['us', 99.2],
    ['who',None],
    ['Nobody',None],
]

bcp_connection = bcp.Connection(
    server = "bdaws154",
    username = 'sa',
    password = 'snoopy',
    database = 'mydb',
    batchsize = 16, # 16 rows before bcp_batch is called, set to 0 to disable
)

try:
    bcp_connection.simplequery('create table [mydb].[dbo].[mytable](name varchar(32), age int)')
except (bcp.DblibError), e:
    print e

bcp_connection.simplequery('truncate table [mydb].[dbo].[mytable]')

bcp_connection.init(
    "[mydb].[dbo].[mytable]"
)

for row in ROW_DATA:
    bcp_connection.send(row)

bcp_connection.done()

bcp_connection.simplequery('select * from [mydb].[dbo].[mytable]', print_results=True)

bcp_connection.disconnect()
