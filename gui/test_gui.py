"""Integration tests against the compiled backend; run python gui/test_gui.py."""
import tempfile
import subprocess
import tkinter as tk
import unittest
from pathlib import Path
from unittest.mock import patch

from backend import execute, EXECUTABLE, DEMO_SETUP, DEMO_JOIN, DEMO_GROUP
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

    def test_cli_file_exit_status(self):
        script = self.directory / 'script.sql'
        for sql, expected in [('select * from missing;', 1), ('exit;', 0),
                              ('show tables;', 0), ('-- comment only\n/* block */;', 0)]:
            with self.subTest(sql=sql):
                script.write_text(sql, encoding='utf-8')
                run = subprocess.run([str(EXECUTABLE), '--data', '.', '--db', 'gui_test',
                                      '-f', script.name], cwd=self.directory, capture_output=True, timeout=20)
                self.assertEqual(run.returncode, expected, run.stdout + run.stderr)

    def test_script_comments(self):
        results = self.sql('-- leading\ncreate table t (id int); /* comment; */;\n'
                           'insert into t values (1); select/* separate tokens */id from t; -- trailing')
        self.assertEqual([r['code'] for r in results], ['SUCCESS'] * 3)
        self.assertEqual(results[-1]['rows'], [['1']])
        self.assertEqual(self.sql('-- only;\n/* block; */;'), [])
        # Comments do not count as an additional statement in EXPLAIN mode.
        plan = execute('explain select * from t; -- trailing', 'gui_test', self.directory, single=True)
        self.assertEqual(plan[0]['code'], 'SUCCESS')

    def test_escaped_quotes_and_comment_markers(self):
        values = ["a';b", "a'--literal", "a'/*literal*/", "a'';b"]
        statements = ['create table t (v char(64))']
        for value in values:
            statements.append("insert into t values ('" + value.replace("'", "\\'") + "')")
        statements.append("insert into t values ('it''s; -- text')")
        statements.append('select * from t')
        result = self.sql(';\n'.join(statements) + ';')
        self.assertTrue(all(r['code'] == 'SUCCESS' for r in result), result)
        self.assertEqual([row[0] for row in result[-1]['rows']], values + ["it's; -- text"])

    def make_app(self):
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        return DatabaseApp(root)

    def test_tk_explain_multiline_and_comments(self):
        self.sql('create table t (id int);')
        app = self.make_app()
        for sql in ['select\n* from t;', 'SELECT\t* FROM t;', '-- comment\nselect * from t;',
                    '/* comment */ select * from t;']:
            with self.subTest(sql=sql), patch.object(app, 'submit') as submit:
                app.load(sql)
                app.explain()
                sent_sql = submit.call_args.args[0]
                self.assertTrue(submit.call_args.kwargs['single'])
                result = execute(sent_sql, 'gui_test', self.directory, single=True)
                self.assertEqual(result[0]['code'], 'SUCCESS')
        result = execute('explain create table unsafe (id int);', 'gui_test', self.directory, single=True)
        self.assertNotEqual(result[0]['code'], 'SUCCESS')
        self.assertEqual(self.sql('show tables;')[0]['rows'], [['t']])

    def test_tk_empty_and_failed_requests_clear_result(self):
        app = self.make_app()
        results = self.sql(DEMO_SETUP)
        app.accept('run', results)
        app.accept('run', [])
        self.assertIsNone(app.current)
        self.assertEqual(app.grid.get_children(), ())
        self.assertEqual(app.selector.get(), '')
        app.accept('run', results)
        app.jobs.put(('run', None, 'backend unavailable'))
        app.poll()
        self.assertIsNone(app.current)
        self.assertEqual(app.grid.get_children(), ())
        self.assertEqual(app.results, [])
        app.accept('plan', self.sql('select * from missing;'))
        self.assertEqual(app.notebook.select(), str(app.messages.master))

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
