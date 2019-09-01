/*=============================================================================
   Copyright (c) 2016-2019 Joel de Guzman

   Distributed under the MIT License (https://opensource.org/licenses/MIT)
=============================================================================*/
#include <elements/app.hpp>
#include <cairo.h>
#include <elements/base_view.hpp>
#include <elements/window.hpp>
#include <elements/support/canvas.hpp>
#include <elements/support/resource_paths.hpp>
#include <elements/support/text_utils.hpp>
#include <json/json_io.hpp>
#include <gtk/gtk.h>
#include <string>

namespace cycfi { namespace elements
{
   struct host_view
   {
      host_view();
      ~host_view();

      cairo_surface_t* surface = nullptr;
      GtkWidget* widget = nullptr;

      // Mouse button click tracking
      std::uint32_t click_time = 0;
      std::uint32_t click_count = 0;

      // Scroll acceleration tracking
      std::uint32_t scroll_time = 0;

      point cursor_position;

      using key_map = std::map<key_code, key_action>;
      key_map keys;

      int modifiers = 0; // the latest modifiers

      GtkIMContext* im_context;
   };

   struct platform_access
   {
      inline static host_view* get_host_view(base_view& view)
      {
         return view.host();
      }
   };

   host_view::host_view()
    : im_context(gtk_im_context_simple_new())
   {
   }

   host_view::~host_view()
   {
      if (surface)
         cairo_surface_destroy(surface);
      surface = nullptr;
   }

   namespace
   {
      inline base_view& get(gpointer user_data)
      {
         return *reinterpret_cast<base_view*>(user_data);
      }

      gboolean on_configure(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data)
      {
         auto& view = get(user_data);
         auto* host_view_h = platform_access::get_host_view(view);

         if (host_view_h->surface)
            cairo_surface_destroy(host_view_h->surface);

         host_view_h->surface = gdk_window_create_similar_surface(
            gtk_widget_get_window(widget), CAIRO_CONTENT_COLOR,
            gtk_widget_get_allocated_width(widget),
            gtk_widget_get_allocated_height(widget)
         );
         return true;
      }

      gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data)
      {
         auto& view = get(user_data);
         auto* host_view_h = platform_access::get_host_view(view);
         cairo_set_source_surface(cr, host_view_h->surface, 0, 0);
         cairo_paint(cr);

         // Note that cr (cairo_t) is already clipped to only draw the
         // exposed areas of the widget.
         double left, top, right, bottom;
         cairo_clip_extents(cr, &left, &top, &right, &bottom);
         view.draw(
            cr,
            rect{ float(left), float(top), float(right), float(bottom) }
         );

         return false;
      }

      template <typename Event>
      bool get_mouse(Event* event, mouse_button& btn, host_view* view)
      {
         btn.modifiers = 0;
         if (event->state & GDK_SHIFT_MASK)
            btn.modifiers |= mod_shift;
         if (event->state & GDK_CONTROL_MASK)
            btn.modifiers |= mod_control | mod_action;
         if (event->state & GDK_MOD1_MASK)
            btn.modifiers |= mod_alt;
         if (event->state & GDK_SUPER_MASK)
            btn.modifiers |= mod_action;

         btn.num_clicks = view->click_count;
         btn.pos = { float(event->x), float(event->y) };
         return true;
      }

      bool get_button(GdkEventButton* event, mouse_button& btn, host_view* view)
      {
         if (event->button > 4)
            return false;

         gint dbl_click_time;
         g_object_get(
            gtk_settings_get_default()
          , "gtk-double-click-time", &dbl_click_time
          , nullptr
         );

         switch (event->type)
         {
            case GDK_BUTTON_PRESS:
               btn.down = true;
               if ((event->time - view->click_time) < dbl_click_time)
                  ++view->click_count;
               else
                  view->click_count = 1;
               view->click_time = event->time;
               break;

            case GDK_BUTTON_RELEASE:
               btn.down = false;
               break;

            default:
               return false;
         }

         if (!get_mouse(event, btn, view))
            return false;
         return true;
      }

      gboolean on_button(GtkWidget* widget, GdkEventButton* event, gpointer user_data)
      {
         auto& view = get(user_data);
         mouse_button btn;
         if (get_button(event, btn, platform_access::get_host_view(view)))
            view.click(btn);
         return true;
      }

      gboolean on_motion(GtkWidget* widget, GdkEventMotion* event, gpointer user_data)
      {
         auto& base_view = get(user_data);
         host_view* view = platform_access::get_host_view(base_view);
         mouse_button btn;
         if (get_mouse(event, btn, view))
         {
            view->cursor_position = btn.pos;

            if (event->state & GDK_BUTTON1_MASK)
            {
               btn.down = true;
               btn.state = mouse_button::left;
            }
            else if (event->state & GDK_BUTTON2_MASK)
            {
               btn.down = true;
               btn.state = mouse_button::middle;
            }
            else if (event->state & GDK_BUTTON3_MASK)
            {
               btn.down = true;
               btn.state = mouse_button::right;
            }
            else
            {
               btn.down = false;
            }

            if (btn.down)
               base_view.drag(btn);
            else
               base_view.cursor(view->cursor_position, cursor_tracking::hovering);
         }
         return true;
      }

      gboolean on_scroll(GtkWidget* widget, GdkEventScroll* event, gpointer user_data)
      {
         auto& base_view = get(user_data);
         auto* host_view_h = platform_access::get_host_view(base_view);
         auto elapsed = std::max<float>(10.0f, event->time - host_view_h->scroll_time);
         static constexpr float _1s = 100;
         host_view_h->scroll_time = event->time;

         float dx = 0;
         float dy = 0;
         float step = _1s / elapsed;

         switch (event->direction)
         {
            case GDK_SCROLL_UP:
               dy = step;
               break;
            case GDK_SCROLL_DOWN:
               dy = -step;
               break;
            case GDK_SCROLL_LEFT:
               dx = step;
               break;
            case GDK_SCROLL_RIGHT:
               dx = -step;
               break;
            case GDK_SCROLL_SMOOTH:
               dx = event->delta_x;
               dy = event->delta_y;
               break;
            default:
               break;
         }

         base_view.scroll(
            { dx, dy },
            { float(event->x), float(event->y) }
         );
         return true;
      }
   }

   gboolean on_event_crossing(GtkWidget* widget, GdkEventCrossing* event, gpointer user_data)
   {
      auto& base_view = get(user_data);
      auto* host_view_h = platform_access::get_host_view(base_view);
      host_view_h->cursor_position = point{ float(event->x), float(event->y) };
      base_view.cursor(
         host_view_h->cursor_position,
         (event->type == GDK_ENTER_NOTIFY) ?
            cursor_tracking::entering :
            cursor_tracking::leaving
      );
      return true;
   }

   // Defined in key.cpp
   key_code translate_key(unsigned key);

   static void on_text_entry(GtkIMContext *context, const gchar* str, gpointer user_data)
   {
      auto& base_view = get(user_data);
      auto* host_view_h = platform_access::get_host_view(base_view);
      auto cp = codepoint(str);
      base_view.text({ cp, host_view_h->modifiers });
   }

   int get_mods(int state)
   {
      int mods = 0;
      if (state & GDK_SHIFT_MASK)
         mods |= mod_shift;
      if (state & GDK_CONTROL_MASK)
         mods |= mod_control | mod_action;
      if (state & GDK_MOD1_MASK)
         mods |= mod_alt;
      if (state & GDK_SUPER_MASK)
         mods |= mod_super;

      return mods;
   }

   void handle_key(base_view& _view, host_view::key_map& keys, key_info k)
   {
      bool repeated = false;

      if (k.action == key_action::release)
      {
         keys.erase(k.key);
         return;
      }

      if (k.action == key_action::press
         && keys[k.key] == key_action::press)
         repeated = true;

      keys[k.key] = k.action;

      if (repeated)
         k.action = key_action::repeat;

      _view.key(k);
   };

   gboolean on_key(GtkWidget* widget, GdkEventKey* event, gpointer user_data)
   {
      auto& base_view = get(user_data);
      auto* host_view_h = platform_access::get_host_view(base_view);
      gtk_im_context_filter_keypress(host_view_h->im_context, event);

      auto const key = translate_key(event->keyval);
      if (key == key_code::unknown)
         return false;

      int modifiers = get_mods(event->state);
      auto const action = event->type == GDK_KEY_PRESS? key_action::press : key_action::release;
      host_view_h->modifiers = modifiers;

      handle_key(base_view, host_view_h->keys, { key, action, modifiers });
      return true;
   }

   gboolean on_focus(GtkWidget* widget, GdkEventFocus* event, gpointer user_data)
   {
      auto& base_view = get(user_data);
      base_view.focus(event->in ?
         focus_request::begin_focus : focus_request::end_focus);
   }

   int poll_function(gpointer user_data)
   {
      auto& base_view = get(user_data);
      base_view.poll();
      return true;
   }

   GtkWidget* make_view(base_view& view, GtkWidget* parent)
   {
      auto* content_view = gtk_drawing_area_new();

      gtk_container_add(GTK_CONTAINER(parent), content_view);

      // Subscribe to content_view events
      g_signal_connect(content_view, "configure-event",
         G_CALLBACK(on_configure), &view);
      g_signal_connect(content_view, "draw",
         G_CALLBACK(on_draw), &view);
      g_signal_connect(content_view, "button-press-event",
         G_CALLBACK(on_button), &view);
      g_signal_connect (content_view, "button-release-event",
         G_CALLBACK(on_button), &view);
      g_signal_connect(content_view, "motion-notify-event",
         G_CALLBACK(on_motion), &view);
      g_signal_connect(content_view, "scroll-event",
         G_CALLBACK(on_scroll), &view);
      g_signal_connect(content_view, "enter-notify-event",
         G_CALLBACK(on_event_crossing), &view);
      g_signal_connect(content_view, "leave-notify-event",
         G_CALLBACK(on_event_crossing), &view);

      gtk_widget_set_events(content_view,
         gtk_widget_get_events(content_view)
         | GDK_BUTTON_PRESS_MASK
         | GDK_BUTTON_RELEASE_MASK
         | GDK_POINTER_MOTION_MASK
         | GDK_SCROLL_MASK
         | GDK_ENTER_NOTIFY_MASK
         | GDK_LEAVE_NOTIFY_MASK
         // | GDK_SMOOTH_SCROLL_MASK
      );

      // Subscribe to parent events
      g_signal_connect(parent, "key-press-event",
         G_CALLBACK(on_key), &view);
      g_signal_connect(parent, "key-release-event",
         G_CALLBACK(on_key), &view);
      g_signal_connect(parent, "focus-in-event",
         G_CALLBACK(on_focus), &view);
      g_signal_connect(parent, "focus-out-event",
         G_CALLBACK(on_focus), &view);

      gtk_widget_set_events(parent,
         gtk_widget_get_events(parent)
         | GDK_KEY_PRESS_MASK
         | GDK_FOCUS_CHANGE_MASK
      );

      // Subscribe to text entry commit
      g_signal_connect(view.host()->im_context, "commit",
         G_CALLBACK(on_text_entry), &view);

      // Create 16ms (60Hz) timer
      g_timeout_add(16, poll_function, &view);

      return content_view;
   }

   // Defined in window.cpp
   GtkWidget* get_window(_host_window& h);
   void on_window_activate(_host_window& h, std::function<void()> f);

   // Defined in app.cpp
   bool app_is_activated();

   struct init_view_class
   {
      init_view_class()
      {
         auto pwd = fs::current_path();
         auto resource_path = pwd / "resources";
         resource_paths.push_back(resource_path);

         canvas::load_fonts(resource_path);
      }
   };

   base_view::base_view(host_view_h h)
    : _view(h)
   {
      static init_view_class init;
   }

   base_view::base_view(host_window h)
    : base_view(new host_view)
   {
      auto make_view =
         [this, h]()
         {
            _view->widget = elements::make_view(*this, get_window(*h));
         };

      if (app_is_activated())
         make_view();
      else
         on_window_activate(*h, make_view);
   }

   base_view::~base_view()
   {
      delete _view;
   }

   point base_view::cursor_pos() const
   {
      return _view->cursor_position;
   }

   elements::size base_view::size() const
   {
      auto x = gtk_widget_get_allocated_width(_view->widget);
      auto y = gtk_widget_get_allocated_height(_view->widget);
      return { float(x), float(y) };
   }

   void base_view::size(elements::size p)
   {
      // $$$ Wrong: don't size the window!!! $$$
      gtk_window_resize(GTK_WINDOW(_view->widget), p.x, p.y);
   }

   void base_view::refresh()
   {
      auto x = gtk_widget_get_allocated_width(_view->widget);
      auto y = gtk_widget_get_allocated_height(_view->widget);
      refresh({ 0, 0, float(x), float(y) });
   }

   void base_view::refresh(rect area)
   {
      auto scale = 1; // get_scale(_view->widget);
      gtk_widget_queue_draw_area(_view->widget,
         area.left * scale,
         area.top * scale,
         area.width() * scale,
         area.height() * scale
      );
   }

   std::string clipboard()
   {
      GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
      gchar* text = gtk_clipboard_wait_for_text(clip);
      return std::string(text);
   }

   void clipboard(std::string const& text)
   {
      GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
      gtk_clipboard_set_text(clip, text.data(), text.size());
   }

   void set_cursor(cursor_type type)
   {
      switch (type)
      {
         case cursor_type::arrow:
            break;
         case cursor_type::ibeam:
            break;
         case cursor_type::cross_hair:
            break;
         case cursor_type::hand:
            break;
         case cursor_type::h_resize:
            break;
         case cursor_type::v_resize:
            break;
      }
   }
}}
