"""Structured bridge to the real database executable, with no third-party packages."""
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = ROOT / 'build' / 'bin' / ('database.exe' if __import__('os').name == 'nt' else 'database')


def execute(sql, database='gui_demo', data_dir=None, profile=True, single=False):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', database):
        raise ValueError('数据库名请使用英文字母、数字和下划线，且不以数字开头。')
    if not EXECUTABLE.is_file():
        raise FileNotFoundError('未找到 build/bin/database，请先执行 bash build.sh。')
    directory = Path(data_dir or ROOT / 'build' / 'gui_data').resolve()
    directory.mkdir(parents=True, exist_ok=True)
    # Use a relative data path: MinGW narrow file APIs cannot open Chinese absolute paths.
    args = [str(EXECUTABLE), '--json-single' if single else '--json', '--db', database, '--data', '.']
    if profile:
        args.append('--profile')
    run = subprocess.run(args, input=sql.encode('utf-8'), stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, cwd=directory, timeout=120,
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    try:
        results = json.loads(run.stdout.decode('utf-8'))
    except (ValueError, UnicodeError) as error:
        detail = run.stderr.decode('utf-8', errors='replace')
        raise RuntimeError('数据库未返回有效 JSON。请重新编译最新程序。\n' + detail) from error
    if run.returncode and not any(item['code'] != 'SUCCESS' for item in results):
        raise RuntimeError('数据库关闭失败：\n' + run.stderr.decode('utf-8', errors='replace'))
    return results


DEMO_SETUP = """-- 演示专用表；重新执行会清空这两张 gui_ 表。
drop table if exists gui_students;
drop table if exists gui_scores;
create table gui_students (id int, term int, name char(24));
create table gui_scores (id int, term int, score int);
insert into gui_students values (1, 1, 'Alice'), (1, 2, 'Alice'), (2, 1, 'Bob'), (3, 1, 'Carol');
insert into gui_scores values (1, 1, 90), (1, 2, 95), (2, 1, 85), (4, 1, 70);
select * from gui_students;
"""
DEMO_JOIN = """select s.id, s.name, s.term, g.score
from gui_students s, gui_scores g
where s.id = g.id and s.term = g.term;"""
DEMO_GROUP = """select s.name, count(*), avg(g.score)
from gui_students s, gui_scores g
where s.id = g.id and s.term = g.term
group by s.name;"""
