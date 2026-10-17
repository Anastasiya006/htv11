class TodoApp {
    constructor() {
        this.todos = JSON.parse(localStorage.getItem('todos')) || [];
        this.filter = 'all';
        
        this.form = document.getElementById('todo-form');
        this.input = document.getElementById('todo-input');
        this.list = document.getElementById('todo-list');
        this.itemsLeft = document.getElementById('items-left');
        this.clearBtn = document.getElementById('clear-completed');
        this.filterBtns = document.querySelectorAll('.filter-btn');

        this.init();
    }

    init() {
        this.form.addEventListener('submit', (e) => {
            e.preventDefault();
            this.addTodo();
        });

        this.clearBtn.addEventListener('click', () => this.clearCompleted());

        this.filterBtns.forEach(btn => {
            btn.addEventListener('click', (e) => {
                this.setFilter(e.target.dataset.filter);
            });
        });

        this.render();
    }

    addTodo() {
        const text = this.input.value.trim();
        if (text) {
            this.todos.push({
                id: Date.now(),
                text,
                completed: false
            });
            this.input.value = '';
            this.saveAndRender();
        }
    }

    toggleTodo(id) {
        this.todos = this.todos.map(todo => 
            todo.id === id ? { ...todo, completed: !todo.completed } : todo
        );
        this.saveAndRender();
    }

    deleteTodo(id) {
        this.todos = this.todos.filter(todo => todo.id !== id);
        this.saveAndRender();
    }

    clearCompleted() {
        this.todos = this.todos.filter(todo => !todo.completed);
        this.saveAndRender();
    }

    setFilter(filter) {
        this.filter = filter;
        this.filterBtns.forEach(btn => {
            btn.classList.toggle('active', btn.dataset.filter === filter);
        });
        this.render();
    }

    saveAndRender() {
        localStorage.setItem('todos', JSON.stringify(this.todos));
        this.render();
    }

    render() {
        const filteredTodos = this.todos.filter(todo => {
            if (this.filter === 'active') return !todo.completed;
            if (this.filter === 'completed') return todo.completed;
            return true;
        });

        this.list.innerHTML = '';
        filteredTodos.forEach(todo => {
            const li = document.createElement('li');
            li.className = `todo-item ${todo.completed ? 'completed' : ''}`;
            li.innerHTML = `
                <div class="checkbox"></div>
                <span>${this.escapeHTML(todo.text)}</span>
                <button class="delete-btn">×</button>
            `;

            li.querySelector('.checkbox').addEventListener('click', () => this.toggleTodo(todo.id));
            li.querySelector('span').addEventListener('click', () => this.toggleTodo(todo.id));
            li.querySelector('.delete-btn').addEventListener('click', () => this.deleteTodo(todo.id));

            this.list.appendChild(li);
        });

        const activeCount = this.todos.filter(t => !t.completed).length;
        this.itemsLeft.textContent = `${activeCount} item${activeCount === 1 ? '' : 's'} left`;
    }

    escapeHTML(str) {
        const p = document.createElement('p');
        p.textContent = str;
        return p.innerHTML;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    new TodoApp();
});
