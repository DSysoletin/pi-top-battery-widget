/*

 * pi-top-battery-widget.c
 * 
 * display pi-top battery status
 * uses /usr/bin/pt-battery to get battery charge information
 *
 * Copyright 2016, 2017  rricharz <rricharz77@gmail.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * Limitations:
 * ************
 *
 * Uses deprecated functions
 *   gtk_status_icon_new_from_pixbuf
 *   gtk_status_icon_set_from_pixbuf
 * 
 * Must be installed at ~/bin, loads battery_icon.png from there
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>




#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include <cairo.h>
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define GRAY_LEVEL      0.93	// Background of % charge display
#define REDLEVEL        10		// below this level battery charge display is red
#define INTERVAL		5000	// msec between two updates

#define MAKELOG         1     // log file batteryLog in home directory (0 = no log file)
#define I2C_SLAVE	0x0703

cairo_surface_t *surface;
gint width;
gint height;

GtkWidget *MainWindow;

guint global_timeout_ref;
int first;
GtkStatusIcon* statusIcon;
FILE *battskript;
FILE *logFile;

int last_capacity, last_state, last_time;

int iconSize;

int i2c_handle=0;
char* device; //I2C bus device
int slave_addr=0x10; //slave(battery controller) address
int bus_delay=2000; //delay after reading/writing, in useconds.
char* buf[10]; //read-write buffer for I2C operations


//Connect to I2C bus and set slave address
int i2c_connect(const char *device, int slave_addr)
{
	i2c_handle=open(device, O_RDWR);
	if(i2c_handle < 0)
	{
		printf("ERROR: Unable to open I2C bus device! Errno: %d Errstr: %s \n",errno, strerror(errno));
		return -1;
	}
	usleep(bus_delay);
	if (ioctl(i2c_handle,I2C_SLAVE,slave_addr)<0)
	{
		printf("ERROR: Unable to set slave address! Errno: %d Errstr: %s \n", errno, strerror(errno));
		return -1;
	}
	usleep(bus_delay);
	//debug
	printf("I2C device opened. Handle: %d \n", i2c_handle);
	return 0;
}

//read from device
//len - is number of bytes to read, can be 1 - for read single byte, or 2 for read word.
char* i2c_read(int i2c_register,int len)
{
	int res;
	//Check len
	if(len<1)
	{
		len=1;
	}
	
	if(len>2)
	{
		len=2;
	}
	//Write register address to device
	res=write(i2c_handle,&i2c_register,1);
	if(res<0)
	{
		printf("ERROR: Error writing to device! Errno: %d Errstr: %s \n", errno, strerror(errno));
	}
	usleep(bus_delay);
	printf("Writing to device finished. Wrote %d bytes. \n",res);
	res = 0;
	//Read data from device
	res=read(i2c_handle,&buf,len);
	if(res<0)
	{
		printf("ERROR: Error reading from device! Errno: %d Errstr: %s \n", errno, strerror(errno));
	}
   usleep(bus_delay);
	//debug
	printf("Reading from device finished. Got %d bytes. \n",res);
	
	return *buf;
}


void printLogEntry(int capacity, char* state, char* timeStr) {
	time_t rawtime;
	struct tm *timeinfo;
	char timeString[80];
	
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(timeString, 80, "%D %R:%S", timeinfo);
	fprintf(logFile, timeString);
	// printf("%s:  %s %d\n", timeString, s, i);
	
	fprintf(logFile, ", Capacity: %3d%%", capacity);
		
	fprintf(logFile, ", %s, %s\n", state, timeStr);
	fflush(logFile);
}

// The following function is called every INTERVAL msec
static gboolean timer_event(GtkWidget *widget)
{
	cairo_t *cr;
	GdkPixbuf *new_pixbuf;
	int w;
	char str[255];
	char timeStr[255];
	
	int capacity, time;
	char *sstatus;
	
	int chargingState;
	char battdata[2048];
	int raw_current=0,current=0;
	
	
	
	// stop timer in case of tc_loop taking too long
	
	g_source_remove(global_timeout_ref);
	
	//read capacity (SoC)
	memset(&buf,0,10);
   i2c_read(0xB4,1);
   //memcpy(&capacity,&buf,1);
   capacity=(int)buf[0];
    //printf("Readed battery SoC: %d \n",buf[0]);
	if ((capacity > 100) || (capacity < 0))
		capacity = -1;              // capacity out of limits

    printf("Readed battery capacity: %d \n",capacity);
		//read current      
      i2c_read(0xB3,2);
	   memcpy(&raw_current,&buf,2);
	   raw_current = ((0xFF & raw_current)<<8) | (raw_current>>8) ; //swap bytes
	   
	   printf("Readed battery raw current: %d \n",raw_current);
	   chargingState=2; //default state - externally powered
	   if(raw_current>32767) //discharging?
	   {
	   	chargingState=0;
	     current=raw_current-65536;
	   }
	   else if(raw_current>0){ //charging?
 	   	chargingState=1;
	   	current=raw_current;
	   }
		printf("Readed battery current: %d mA \n",current);

	
	
	// check whether state or capacity has changed
	if ((last_state == chargingState) && (last_capacity == capacity)) {
		// restart timer and return without updating icon
		global_timeout_ref = g_timeout_add(INTERVAL, (GSourceFunc) timer_event, (gpointer) MainWindow);	
		return TRUE;		
	}
	
	sstatus = "no battery";
	if (chargingState == 0)
		sstatus = "discharging";
	else if (chargingState == 1)
		sstatus = "charging";
	else if (chargingState == 2)
		sstatus = "externally powered";
		
	// prepare tooltip
	sprintf(timeStr, "%s", sstatus);
	if (strcmp(sstatus,"charging") == 0) {
		if ((time > 0) && (time < 1000)) {
			if (time <= 90) {
				sprintf(timeStr, "Charging time: %d minutes", time);
			}
			else {
				sprintf(timeStr, "Charging time: %.1f hours", (float)time / 60.0);  
			}
		}
	}
	else if (strcmp(sstatus,"discharging") == 0) {
		if ((time > 0) && (time < 1000)) {
			if (time <= 90) {
				sprintf(timeStr, "Time remaining: %d minutes", time);
			}
			else {
				sprintf(timeStr, "Time remaining: %.1f hours", (double)time / 60.0);  
			}
		} 
	}
	
	// update log
	
	printLogEntry(capacity, sstatus, timeStr);
		
	// create a drawing surface
		
	cr = cairo_create (surface);
	
	// scale surface, if necessary
	
	if (width != iconSize) {
		double scaleFactor = (double) iconSize / (double) width;
		if (iconSize >= 39) {
			double scaleFactor = (double) (iconSize - 3) / (double) width;
			cairo_translate(cr, 0.0, 3.0);
		}
		else {
			double scaleFactor = (double) iconSize / (double) width;
		}
		cairo_scale(cr, scaleFactor,scaleFactor);
	}
	
	// fill the battery symbol with a colored bar
	
	if (capacity < 0)         // capacity out of limits
	  w = 0;
	else
	  w = (99 * capacity) / 400;
	if (strcmp(sstatus,"charging") == 0)
		cairo_set_source_rgb (cr, 1, 1, 0);
	else if (capacity <= REDLEVEL)
		cairo_set_source_rgb (cr, 1, 0, 0);
	else if (strcmp(sstatus,"externally powered") == 0)
	    cairo_set_source_rgb (cr, 0.5, 0.5, 0.7);
	else
		cairo_set_source_rgb (cr, 0, 1, 0);
	cairo_rectangle (cr, 5, 4, w, 12);
	cairo_fill (cr);
	if (w < 23) {
		cairo_set_source_rgb (cr, 1, 1, 1);
		cairo_rectangle (cr, 5 + w, 4, 24 - w, 12);
		cairo_fill (cr);
	}
	
	// display the capacity figure
	
	cairo_set_source_rgb (cr, GRAY_LEVEL, GRAY_LEVEL, GRAY_LEVEL);
	cairo_rectangle (cr, 0, 20, 35, 15);
	cairo_fill (cr);  
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_select_font_face(cr, "Dosis", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12);
	if (capacity >= 0) {
	  int x = 4;
	  if (capacity < 10)
	    x += 4;
	  else if (capacity > 99)
	    x -= 4;
	  cairo_move_to(cr, x, 33);
	  sprintf(str,"%2d%%",capacity);
	  cairo_show_text(cr, str);
	}
		
	// Create a new pixbuf from the modified surface and display icon
	
	new_pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, iconSize, iconSize);
	
	if (first) {
		statusIcon= gtk_status_icon_new_from_pixbuf(new_pixbuf);
		first = FALSE;
	}
	else {
		gtk_status_icon_set_from_pixbuf(statusIcon, new_pixbuf);
	}
	gtk_status_icon_set_tooltip_text(statusIcon, timeStr);
	    
	g_object_unref(new_pixbuf);
	cairo_destroy (cr);
	
	last_capacity = capacity;
	last_state = chargingState;
	last_time = time;
	
	// restart timer
			
	global_timeout_ref = g_timeout_add(INTERVAL, (GSourceFunc) timer_event, (gpointer) MainWindow);
	
	return TRUE;
}

// This function is called once at startup to initialize everything

int main(int argc, char *argv[])
{
	GdkPixbuf *pixbuf, *new_pixbuf;
	cairo_t *cr;
	cairo_format_t format;
	
	first = TRUE;
	last_capacity = -1;
	last_state = -1;
	last_time = -1;
	
	gtk_init(&argc, &argv);
	
	device="/dev/i2c-1"; //I2C bus device
	
	// open log file
	
	if(i2c_connect(device,slave_addr)!=0)
	{
		printf("Error connecting to i2c device! \n");
		return(0);
	}
	
	
	const char *homedir = getpwuid(getuid())->pw_dir;	
	char s[255];
	if (MAKELOG) {
		strcpy(s, homedir);
		strcat(s, "/batteryLog.txt" );
		// printf("s = %s\n",s);
		logFile = fopen(s,"a");
		fprintf(logFile, "Starting pi-top-battery-widget\n");
	}
	else
		logFile = stdout;
	
	// get lxpanel icon size
	iconSize = -1;
	strcpy(s, homedir);
	strcat(s, "/.config/lxpanel/LXDE/panels/panel");
	FILE* lxpanel = fopen(s, "r");
	if (lxpanel == NULL) {
		printf("Failed to open lxpanel config file %s\n", s);
		fprintf(logFile,"Failed to open lxpanel config file %s\n", s);
	}
	else {
		char lxpaneldata[2048];
		while ((fgets(lxpaneldata, 2047, lxpanel) != NULL) && (iconSize == -1)) {
			sscanf(lxpaneldata,"  iconsize=%d", &iconSize);
		}
		fclose(lxpanel);
	}
	if (iconSize == -1)    // default
		iconSize = 36;
	// printf("lxpanel iconSize = %d\n", iconSize);

	// create the drawing surface and fill with icon

	strcpy(s, homedir);
	strcat(s, "/bin/battery_icon.png" );
	// printf("s = %s\n",s);
	pixbuf = gdk_pixbuf_new_from_file (s, NULL);
	if (pixbuf == NULL) {
		printf("Cannot load icon (%s)\n", s);
		fprintf(logFile,"Cannot load icon (%s)\n", s);
		return 1;
	}
	format = (gdk_pixbuf_get_has_alpha (pixbuf)) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
	height = gdk_pixbuf_get_height (pixbuf);
	width = gdk_pixbuf_get_width (pixbuf);
	surface = cairo_image_surface_create (format, iconSize, iconSize);
	g_assert (surface != NULL);
	
	cr = cairo_create (surface);
	
	// scale surface, if necessary
	
	if (width != iconSize) {
		double scaleFactor = (double) iconSize / (double) width;
		if (iconSize >= 39) {
			double scaleFactor = (double) (iconSize - 3) / (double) width;
			cairo_translate(cr, 0.0, 3.0);
		}
		else {
			double scaleFactor = (double) iconSize / (double) width;
		}
		cairo_scale(cr, scaleFactor,scaleFactor);
	}
	
	// Draw icon onto the surface
	 
	gdk_cairo_set_source_pixbuf (cr, pixbuf, 0, 0);
	cairo_paint (cr);
	cairo_destroy (cr);
	
	// Add timer event
	// Register the timer and set time in mS.
	// The timer_event() function is called repeatedly.
	 
	global_timeout_ref = g_timeout_add(INTERVAL, (GSourceFunc) timer_event, (gpointer) MainWindow);

	// Call the timer function because we don't want to wait for the first time period triggered call

	timer_event(MainWindow);
	
	gtk_main();
	
	 close(i2c_handle);
	
	return 0;
}
