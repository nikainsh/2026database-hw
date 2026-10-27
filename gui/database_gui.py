"""Run with: python gui/database_gui.py (Python standard library only)."""
import csv
import queue
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

from backend import ROOT, execute, DEMO_SETUP, DEMO_JOIN, DEMO_GROUP


class DatabaseApp:
    def __init__(self, root):
        self.root = root
        root.title('database · 数据库实验工作台')
        root.geometry('1180x800')
        root.minsize(900, 650)
        style = ttk.Style(root)
        style.theme_use('clam')
        style.configure('.', font=('Microsoft YaHei UI', 10))
        style.configure('Treeview', rowheight=28)
        style.configure('Title.TLabel', font=('Microsoft YaHei UI', 19, 'bold'))
        style.configure('Accent.TButton', foreground='#ffffff', background='#2563eb', padding=8)
        self.database = tk.StringVar(value='gui_demo')
        self.directory = tk.StringVar(value=str(ROOT / 'build' / 'gui_data'))
        self.profile = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value='就绪 · 先点击“加载示例：建表”，检查 SQL 后运行')
        self.jobs = queue.Queue()
        self.busy = False
        self.results = []
        self.current = None
        self.history = []
        page = ttk.Frame(root, padding=18)
        page.pack(fill='both', expand=True)
        ttk.Label(page, text='database  数据库实验工作台', style='Title.TLabel').pack(anchor='w')
        ttk.Label(page, text='SQL 查询  /  执行计划  /  算子统计', foreground='#64748b').pack(anchor='w', pady=(3, 14))
        connection = ttk.Frame(page)
        connection.pack(fill='x')
        ttk.Label(connection, text='数据库').pack(side='left')
        ttk.Entry(connection, textvariable=self.database, width=18).pack(side='left', padx=8)
        ttk.Label(connection, text='数据目录').pack(side='left')
        ttk.Entry(connection, textvariable=self.directory).pack(side='left', fill='x', expand=True, padx=8)
        ttk.Button(connection, text='选择目录', command=self.choose_directory).pack(side='left')
        body = ttk.Panedwindow(page, orient='horizontal')
        body.pack(fill='both', expand=True, pady=12)
        sidebar = ttk.Frame(body, padding=(0, 0, 12, 0))
        body.add(sidebar, weight=1)
        ttk.Label(sidebar, text='数据库对象', font=('Microsoft YaHei UI', 11, 'bold')).pack(anchor='w')
        self.refresh_button = ttk.Button(sidebar, text='连接 / 刷新表', command=self.refresh)
        self.refresh_button.pack(fill='x', pady=8)
        self.tables = tk.Listbox(sidebar, width=21, font=('Consolas', 11), borderwidth=0, highlightthickness=1)
        self.tables.pack(fill='both', expand=True)
        self.tables.bind('<Double-Button-1>', self.describe_table)
        ttk.Label(sidebar, text='双击表名查看结构\n数据保存在所选目录\n默认使用独立演示库', foreground='#64748b').pack(anchor='w', pady=10)
        work = ttk.Frame(body)
        body.add(work, weight=5)
        toolbar = ttk.Frame(work)
        toolbar.pack(fill='x', pady=(0, 8))
        self.run_button = ttk.Button(toolbar, text='▶ 运行 SQL', style='Accent.TButton', command=self.run)
        self.run_button.pack(side='left')
        self.plan_button = ttk.Button(toolbar, text='查看 EXPLAIN', command=self.explain)
        self.plan_button.pack(side='left', padx=8)
        ttk.Checkbutton(toolbar, text='采集执行统计', variable=self.profile).pack(side='left')
        ttk.Button(toolbar, text='导出当前结果 CSV', command=self.export).pack(side='right')
        demos = ttk.Frame(work)
        demos.pack(fill='x', pady=(0, 8))
        ttk.Label(demos, text='加载示例：').pack(side='left')
        for name, sql in [('建表及数据', DEMO_SETUP), ('联合键 Hash Join', DEMO_JOIN), ('连接与分组', DEMO_GROUP)]:
            ttk.Button(demos, text=name, command=lambda s=sql: self.load(s)).pack(side='left', padx=3)
        ttk.Label(work, text='SQL 编辑器  ·  Ctrl+Enter 运行所选 SQL；无选区时运行全文').pack(anchor='w')
        self.editor = tk.Text(work, height=10, wrap='none', font=('Consolas', 12), bg='#111827',
                              fg='#e2e8f0', insertbackground='white', padx=12, pady=10, undo=True)
        self.editor.pack(fill='both', expand=True, pady=(5, 10))
        self.editor.bind('<Control-Return>', lambda event: self.run())
        self.load(DEMO_SETUP)
        self.notebook = ttk.Notebook(work)
        self.notebook.pack(fill='both', expand=True)
        self.result_page = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(self.result_page, text=' 查询结果 ')
        self.selector = ttk.Combobox(self.result_page, state='readonly')
        self.selector.pack(fill='x', pady=(0, 5))
        self.selector.bind('<<ComboboxSelected>>', lambda event: self.show_result(self.selector.current()))
        grid = ttk.Frame(self.result_page)
        grid.pack(fill='both', expand=True)
        self.grid = ttk.Treeview(grid, show='headings')
        vs = ttk.Scrollbar(grid, orient='vertical', command=self.grid.yview)
        hs = ttk.Scrollbar(grid, orient='horizontal', command=self.grid.xview)
        self.grid.configure(yscrollcommand=vs.set, xscrollcommand=hs.set)
        self.grid.grid(row=0, column=0, sticky='nsew')
        vs.grid(row=0, column=1, sticky='ns')
        hs.grid(row=1, column=0, sticky='ew')
        grid.rowconfigure(0, weight=1)
        grid.columnconfigure(0, weight=1)
        self.plan = self.text_tab(' 执行计划 ')
        self.statistics = self.text_tab(' 执行统计 ')
        self.messages = self.text_tab(' 消息 / 历史 ')
        ttk.Label(page, textvariable=self.status, foreground='#2563eb', wraplength=1100).pack(anchor='w')
        root.after(80, self.poll)

    def text_tab(self, name):
        frame = ttk.Frame(self.notebook)
        self.notebook.add(frame, text=name)
        text = tk.Text(frame, wrap='none', font=('Consolas', 10), padx=12, pady=10)
        scroll = ttk.Scrollbar(frame, orient='vertical', command=text.yview)
        text.configure(yscrollcommand=scroll.set, state='disabled')
        scroll.pack(side='right', fill='y')
        horizontal = ttk.Scrollbar(frame, orient='horizontal', command=text.xview)
        text.configure(xscrollcommand=horizontal.set)
        horizontal.pack(side='bottom', fill='x')
        text.pack(fill='both', expand=True)
        return text

    @staticmethod
    def replace(widget, content):
        widget.configure(state='normal')
        widget.delete('1.0', 'end')
        widget.insert('1.0', content)
        widget.configure(state='disabled')

    def choose_directory(self):
        directory = filedialog.askdirectory(parent=self.root)
        if directory:
            self.directory.set(directory)

    def load(self, sql):
        self.editor.delete('1.0', 'end')
        self.editor.insert('1.0', sql)

    def sql(self):
        try:
            return self.editor.get('sel.first', 'sel.last').strip()
        except tk.TclError:
            return self.editor.get('1.0', 'end').strip()

    def run(self):
        self.submit(self.sql(), 'run')
        return 'break'

    def explain(self):
        sql = self.sql().rstrip(';').strip()
        # Let the SQL parser reject multiple statements; never execute trailing DML here.
        if not sql.lower().startswith('select '):
            messagebox.showinfo('EXPLAIN', '请选择一条以 SELECT 开头的查询，再查看计划。')
            return
        self.submit('explain ' + sql, 'plan', single=True)

    def refresh(self):
        self.submit('show tables;', 'tables')

    def describe_table(self, event=None):
        selected = self.tables.curselection()
        if selected:
            name = self.tables.get(selected[0])
            self.load('desc ' + name + ';')
            self.run()

    def submit(self, sql, kind, single=False):
        if self.busy or not sql:
            return
        self.busy = True
        self.status.set('正在执行… SQL 错误将停止本次脚本，已成功的语句不会回滚。')
        for button in (self.run_button, self.plan_button, self.refresh_button):
            button.configure(state='disabled')
        database, directory, profile = self.database.get(), self.directory.get(), self.profile.get()
        def work():
            try:
                self.jobs.put((kind, execute(sql, database, directory, profile, single), None))
            except Exception as error:
                self.jobs.put((kind, None, str(error)))
        threading.Thread(target=work, daemon=True).start()

    def poll(self):
        try:
            kind, results, error = self.jobs.get_nowait()
        except queue.Empty:
            pass
        else:
            self.busy = False
            for button in (self.run_button, self.plan_button, self.refresh_button):
                button.configure(state='normal')
            if error:
                self.status.set('执行失败')
                self.history.append(error)
                self.replace(self.messages, '\n\n'.join(self.history))
                self.notebook.select(self.messages.master)
            else:
                self.accept(kind, results)
        self.root.after(80, self.poll)

    def accept(self, kind, results):
        self.results = results
        for item in results:
            self.history.append(f"{item['sql'].strip()}\n[{item['code']}] {item['message']} | "
                                f"{item['total']} rows | {item['time_us']} us")
        self.history = self.history[-100:]
        self.replace(self.messages, '\n\n'.join(self.history))
        self.selector['values'] = [f"{i+1}. [{r['code']}] {r['sql'].strip()[:75]}" for i, r in enumerate(results)]
        if not results:
            self.status.set('没有可执行的 SQL')
            return
        self.selector.current(len(results)-1)
        self.show_result(len(results)-1)
        if kind == 'tables' and results[-1]['code'] == 'SUCCESS':
            self.tables.delete(0, 'end')
            for row in results[-1]['rows']:
                self.tables.insert('end', row[0])
        if kind == 'plan':
            self.notebook.select(self.plan.master)
        elif results[-1]['code'] != 'SUCCESS':
            self.notebook.select(self.messages.master)
        else:
            self.notebook.select(self.result_page)

    def show_result(self, index):
        if index < 0 or index >= len(self.results):
            return
        result = self.current = self.results[index]
        self.grid.delete(*self.grid.get_children())
        ids = [str(i) for i in range(len(result['columns']))]
        self.grid['columns'] = ids
        for key, label in zip(ids, result['columns']):
            self.grid.heading(key, text=label)
            self.grid.column(key, width=160, minwidth=100, stretch=True)
        for row in result['rows']:
            self.grid.insert('', 'end', values=row)
        plan = '\n'.join(row[0] for row in result['rows']) if result['columns'] == ['plan'] else '点击“查看 EXPLAIN”查看当前查询的逻辑计划和物理计划。'
        self.replace(self.plan, plan)
        stats = ('EXPLAIN 仅生成计划。请执行原 SELECT 查询后查看实际算子统计。'
                 if result['columns'] == ['plan'] else result['profile'])
        self.replace(self.statistics, stats or '本语句没有执行算子统计。请勾选采集统计，然后执行 SELECT。')
        self.status.set(f"[{result['code']}] 返回 {result['total']} 行 · 显示 {len(result['rows'])} 行（最多 1000） · "
                        f"耗时 {result['time_us']} μs · {result['message']}")

    def export(self):
        if not self.current or not self.current['columns']:
            messagebox.showinfo('导出', '请先运行一条有结果列的查询。')
            return
        path = filedialog.asksaveasfilename(defaultextension='.csv', filetypes=[('CSV', '*.csv')])
        if path:
            try:
                with open(path, 'w', newline='', encoding='utf-8-sig') as output:
                    writer = csv.writer(output)
                    writer.writerow(self.current['columns'])
                    writer.writerows(self.current['rows'])
                self.status.set('已导出当前显示的结果（最多 1000 行）：' + path)
            except OSError as error:
                messagebox.showerror('导出失败', str(error))


if __name__ == '__main__':
    window = tk.Tk()
    DatabaseApp(window)
    window.mainloop()
