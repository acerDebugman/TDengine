from new_test_framework.utils import tdLog, tdSql, sc, clusterComCheck


class TestNormalTableDouble:

    def setup_class(cls):
        tdLog.debug(f"start to execute {__file__}")

    def test_normal_table_double(self):
        """create normal table (double)

        1. create normal table
        2. insert data
        3. query from normal table

        Catalog:
            - Table:NormalTable:Create

        Since: v3.0.0.0

        Labels: common,ci

        Jira: None

        History:
            - 2025-4-28 Simon Guan Migrated from tsim/table/double.sim

        """
        i = 0
        dbPrefix = "lm_do_db"
        tbPrefix = "lm_do_tb"
        db = dbPrefix + str(i)
        tb = tbPrefix + str(i)

        tdLog.info(f"=============== step1")
        tdSql.prepare(dbname=db)
        tdSql.execute(f"use {db}")
        tdSql.execute(f"create table {tb} (ts timestamp, speed double)")

        tdLog.info(f"=============== step2")

        tdSql.query(f"select * from {tb} order by ts desc")
        tdSql.checkRows(0)

        tdLog.info(f"=============== step3")
        tdSql.execute(f"insert into {tb} values (now+2a, 2.85)")
        tdSql.query(f"select * from {tb} order by ts desc")
        tdSql.checkRows(1)

        tdSql.checkData(0, 1, 2.850000000)

        tdLog.info(f"=============== step4")
        tdSql.execute(f"insert into {tb} values (now+3a, 3.4)")
        tdSql.query(f"select * from {tb} order by ts desc")
        tdSql.checkRows(2)

        tdSql.checkData(0, 1, 3.400000000)

        tdLog.info(f"=============== step5")
        tdSql.error(f"insert into {tb} values (now+4a, a2) ")
        tdSql.execute(f"insert into {tb} values (now+4a, 0)")
        tdSql.query(f"select * from {tb} order by ts desc")
        tdSql.checkRows(3)

        tdSql.checkData(0, 1, 0.000000000)

        tdLog.info(f"=============== step6")
        tdSql.error(f"insert into {tb} values (now+5a, 2a) ")
        tdSql.execute(f"insert into {tb} values(now+5a, 2)")
        tdSql.query(f"select * from {tb} order by ts desc")
        tdSql.checkRows(4)

        tdSql.checkData(0, 1, 2.000000000)

        tdLog.info(f"=============== step7")
        tdSql.error(f"insert into {tb} values (now+6a, 2a'1)")
        tdSql.execute(f"insert into {tb} values(now+6a, 2)")
        tdSql.query(f"select * from {tb} order by ts desc")
        tdSql.checkRows(5)

        tdSql.checkData(0, 1, 2.000000000)

        tdSql.execute(f"drop database {db}")
        tdSql.query(f"select * from information_schema.ins_databases")
        tdSql.checkRows(2)
