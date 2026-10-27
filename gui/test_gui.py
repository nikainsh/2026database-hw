"""Integration tests against the compiled backend; run python gui/test_gui.py."""
import tempfile
import tkinter as tk
import unittest
from pathlib import Path

from backend import execute, DEMO_SETUP, DEMO_JOIN, DEMO_GROUP
from database_gui import DatabaseApp


class GuiTests(unittest.TestCase):
    def setUp(self):
        # Under build: exercise Chinese ancestor paths while keeping test databases isolated.
        from backend import ROOT
        (ROOT / 'build').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='gui_test_')
        self.directory = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def sql(self, sql, profile=True):
        return execute(sql, 'gui_test', self.directory, profile)

    def test_join_plan_and_profile(self):
        self.assertTrue(all(r['code'] == 'SUCCESS' for r in self.sql(DEMO_SETUP)))
        result = self.sql(DEMO_JOIN)[0]
        self.assertEqual(result['total'], 3)
        self.assertEqual(sorted(result['rows']), [['1', 'Alice', '1', '90'], ['1', 'Alice', '2', '95'], ['2', 'Bob', '1', '85']])
        self.assertIn('hash_join', result['profile'])
        plan = '\n'.join(row[0] for row in self.sql('explain ' + DEMO_JOIN)[0]['rows'])
        self.assertIn('key_count=2', plan)
        self.assertEqual(self.sql(DEMO_GROUP)[0]['total'], 2)

    def test_escaping_and_utf8(self):
        text = '中文|分号;换行\n双引号"反斜线\\内容'
        result = self.sql("create table t (v char(100)); insert into t values ('" + text + "'); select * from t;")
        self.assertEqual(result[-1]['rows'], [[text]])
        self.assertEqual(result[1]['affected'], 1)

    def test_script_stops_on_error(self):
        result = self.sql('select * from missing; create table should_not_exist (id int);')
        self.assertEqual(len(result), 1)
        self.assertNotEqual(result[0]['code'], 'SUCCESS')
        self.assertEqual(self.sql('show tables;')[0]['total'], 0)

    def test_row_cap_and_exact_count(self):
        values = ','.join(f'({i})' for i in range(1005))
        result = self.sql('create table t (id int); insert into t values ' + values + '; select * from t;')[-1]
        self.assertEqual(result['total'], 1005)
        self.assertEqual(len(result['rows']), 1000)

    def test_empty_and_profile_off(self):
        result = self.sql('create table t (id int); select * from t;', profile=False)[-1]
        self.assertEqual(result['columns'], ['t.id'])
        self.assertEqual(result['rows'], [])
        self.assertEqual(result['profile'], '')

    def test_numeric_error(self):
        result = self.sql('create table t (v bigint); insert into t values (9223372036854775807), (1); select sum(v) from t;')[-1]
        self.assertEqual(result['code'], 'NUMERIC_OVERFLOW')

    def test_explain_cannot_run_trailing_writes(self):
        result = execute('explain select * from missing; create table unsafe (id int);',
                         'gui_test', self.directory, single=True)
        self.assertEqual(result[0]['code'], 'INVALID_ARGUMENT')
        self.assertEqual(self.sql('show tables;')[0]['total'], 0)

    def test_tk_result_switching(self):
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        app = DatabaseApp(root)
        results = self.sql(DEMO_SETUP)
        app.accept('run', results)
        root.update_idletasks()
        self.assertEqual(len(app.grid.get_children()), 4)
        app.show_result(0)
        self.assertEqual(len(app.grid.get_children()), 0)
        app.accept('tables', self.sql('show tables;'))
        self.assertEqual(set(app.tables.get(0, 'end')), {'gui_students', 'gui_scores'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
